// URL shortener — short codes from a keyed Feistel permutation, a SQLite store behind an LRU, malicious-domain
// screening with a Bloom filter, and click analytics from sketches, all driven over HTTP by its own traffic.
//
// WHAT IT IS
//   bit.ly in one file, with the parts that make it hard rather than the parts that make it a demo:
//
//   codes       sequence numbers go through a 4-round Feistel network keyed by HMAC, then base62: codes are short,
//               non-sequential (nobody can enumerate links) and collision-free by construction — the permutation is
//               checked to be a bijection on a 2^20 domain
//   screening   50 000 known-bad domains in a BloomFilter (5 bits/key would leak 9% false positives; this one is
//               sized for ~1%); a Bloom hit is confirmed against the exact list, so a false positive costs a
//               lookup, never a refused link; IDN hosts are compared in punycode (domainToASCII)
//   serving     GET /:code is a 301 from an LRU in front of SQLite; expiring links leave via an IndexedHeap
//               sweep and answer 410; a native RateLimiter throttles link creation per client
//   analytics   per-link unique visitors in a HyperLogLog, hot links in a TopK, referrers in a CountMinSketch;
//               each estimate is checked against exact counts kept on the side
//
// MODULES  dyna:http (App, fetch) · dyna:net (SQLite, RateLimiter) · dyna:structures (BloomFilter, LRU, HyperLogLog,
//          TopK, CountMinSketch, IndexedHeap, Multiset) · dyna:crypto (HMAC) · dyna:url (URL, domainToASCII) ·
//          dyna:random · dyna:cli
//
// RUN      dynajs examples/web/url-shortener.js

import { App } from "dyna:http";
import { SQLite, RateLimiter } from "dyna:net";
import { BloomFilter, LRU, HyperLogLog, TopK, CountMinSketch, IndexedHeap, Multiset } from "dyna:structures";
import { HMAC } from "dyna:crypto";
import { URL, domainToASCII } from "dyna:url";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(62);

// =============================================================================
// Codes: a keyed Feistel permutation over 2 x 20-bit halves, rendered in base62
// =============================================================================
const KEY = "shortener-code-key";
const roundKeys = [0, 1, 2, 3].map((r) => {
    const h = HMAC("sha256", KEY, `round-${r}`);
    return (h[0] << 16) | (h[1] << 8) | h[2];
});
const F = (x, k) => (Math.imul(x ^ k, 0x45d9f3b) >>> 12) & 0xfffff;
function permute(n, halfBits = 20) {
    const mask = (1 << halfBits) - 1;
    let l = Math.floor(n / 2 ** halfBits) & mask,
        r = n & mask;
    for (const k of roundKeys) [l, r] = [r, (l ^ F(r, k)) & mask];
    return l * 2 ** halfBits + r;
}
function unpermute(n, halfBits = 20) {
    const mask = (1 << halfBits) - 1;
    let l = Math.floor(n / 2 ** halfBits) & mask,
        r = n & mask;
    for (const k of [...roundKeys].reverse()) [l, r] = [(r ^ F(l, k)) & mask, l];
    return l * 2 ** halfBits + r;
}
const B62 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
const toB62 = (n) => {
    let s = "";
    do {
        s = B62[n % 62] + s;
        n = Math.floor(n / 62);
    } while (n > 0);
    return s.padStart(7, "0");
};
const fromB62 = (s) => [...s].reduce((n, c) => n * 62 + B62.indexOf(c), 0);
{
    // A Feistel network is a bijection whatever F is; check it on a 2^20 domain (10-bit halves) exhaustively.
    const seen = new Uint8Array(1 << 20);
    let collisions = 0,
        inverseOk = true;
    for (let i = 0; i < 1 << 20; i++) {
        const p = permute(i, 10);
        if (seen[p]++) collisions++;
        if (i % 4099 === 0 && unpermute(p, 10) !== i) inverseOk = false;
    }
    results["Feistel permutation is a bijection on 2^20 values"] = collisions === 0 && inverseOk;
    const firstCodes = [1, 2, 3].map((n) => toB62(permute(n)));
    results["consecutive ids give unrelated codes"] = new Set(firstCodes.map((c) => c.slice(0, 3))).size === 3;
    results["codes decode back to their sequence number"] = [1, 2, 99999].every(
        (n) => unpermute(fromB62(toB62(permute(n)))) === n,
    );
}

// =============================================================================
// Screening list
// =============================================================================
const badDomains = Array.from({ length: 50_000 }, (_, i) => `malware-${i.toString(36)}.example`);
badDomains.push(domainToASCII("phish-bänk.example"));
const bloom = new BloomFilter(50_000 * 10, 7),
    exactBad = new Set(badDomains);
for (const d of badDomains) bloom.add(d);
let bloomFp = 0;
for (let i = 0; i < 20_000; i++) if (bloom.mayContain(`benign-${i}.example`)) bloomFp++;
results["Bloom false-positive rate near 1%"] = bloomFp / 20_000 < 0.015;

// =============================================================================
// Service
// =============================================================================
const db = new SQLite(":memory:");
db.exec("CREATE TABLE links (id INTEGER PRIMARY KEY, code TEXT UNIQUE, url TEXT, owner TEXT, expires INTEGER)");
const hot = new LRU(1000),
    expiries = new IndexedHeap();
const visitors = new Map(),
    exactVisitors = new Map(),
    topLinks = new TopK(100),
    exactClicks = new Multiset();
const referrers = new CountMinSketch(4096, 5),
    exactReferrers = new Multiset();
const limiter = new RateLimiter({ tokensPerSec: 50, burst: 200 });
let nextSeq = 1,
    now = Date.UTC(2026, 9, 25),
    dbReads = 0,
    bloomConfirms = 0;

function createLink(body, client) {
    if (!limiter.allow(client)) return { status: 429, body: "slow down" };
    let req;
    try {
        req = JSON.parse(body);
    } catch {
        return { status: 400, body: "body must be JSON" };
    }
    let u;
    try {
        u = new URL(req.url);
    } catch {
        return { status: 400, body: "invalid url" };
    }
    if (!["http:", "https:"].includes(u.protocol)) return { status: 400, body: "only http(s) links" };
    const host = domainToASCII(u.hostname);
    if (bloom.mayContain(host)) {
        bloomConfirms++;
        if (exactBad.has(host)) return { status: 403, body: "destination is on the blocklist" };
    }
    let code;
    if (req.alias) {
        code = req.alias.toLowerCase().replace(/[^a-z0-9-]/g, "-");
        if (db.query("SELECT 1 FROM links WHERE code = ?", [code]).length) return { status: 409, body: "alias taken" };
    } else code = toB62(permute(nextSeq));
    const id = nextSeq++;
    const expires = req.ttlSeconds ? now + req.ttlSeconds * 1000 : null;
    db.exec("INSERT INTO links VALUES (?, ?, ?, ?, ?)", [id, code, u.href, client, expires]);
    if (expires) expiries.push(id, expires);
    return { status: 201, body: JSON.stringify({ code, short: `/${code}`, url: u.href }) };
}
function sweep() {
    for (const id of expiries.popUntil(now)) {
        const row = db.query("SELECT code FROM links WHERE id = ?", [id])[0];
        if (row) hot.delete(row.code);
        db.exec("UPDATE links SET url = NULL WHERE id = ?", [id]);
    }
}
function follow(req) {
    sweep();
    const code = req.params.code;
    let url = hot.get(code);
    if (url === undefined) {
        dbReads++;
        const row = db.query("SELECT url FROM links WHERE code = ?", [code])[0];
        if (!row) return { status: 404, body: "unknown link" };
        if (row.url === null) return { status: 410, body: "link expired" };
        url = row.url;
        hot.set(code, url);
    }
    const visitor = `${req.headers["x-forwarded-for"]}|${req.headers["user-agent"]}`;
    (visitors.get(code) ?? visitors.set(code, new HyperLogLog(12)).get(code)).add(visitor);
    (exactVisitors.get(code) ?? exactVisitors.set(code, new Set()).get(code)).add(visitor);
    topLinks.add(code);
    exactClicks.add(code);
    const ref = req.headers.referer ?? "(direct)";
    referrers.add(`${code}|${ref}`);
    exactReferrers.add(`${code}|${ref}`);
    return { status: 301, headers: { location: url }, body: "" };
}

const app = new App({ port: 0 });
app.post("/links", (req) => createLink(req.body, req.headers["x-client"] ?? "anon"));
app.get("/stats/:code", (req) => {
    const h = visitors.get(req.params.code);
    return h
        ? { uniques: Math.round(h.count()), clicks: topLinks.count(req.params.code) }
        : { status: 404, body: "no stats" };
});
app.get("/:code", follow);
app.start();
const base = `http://127.0.0.1:${app.port}`;
const post = (body, client = "tester") =>
    fetch(base + "/links", {
        method: "POST",
        body: typeof body === "string" ? body : JSON.stringify(body),
        headers: { "x-client": client },
    });

// =============================================================================
// Traffic
// =============================================================================
const created = [];
for (let batch = 0; batch < 10; batch++) {
    const rs = await Promise.all(
        Array.from({ length: 100 }, (_, i) =>
            post({ url: `https://shop.example/item/${batch * 100 + i}?ref=campaign` }, `client-${i % 7}`),
        ),
    );
    for (const r of rs) if (r.status === 201) created.push(await r.json());
    now += 1000;
}
results["1000 links created"] = created.length === 1000 && new Set(created.map((c) => c.code)).size === 1000;
results["every code is 7 base62 characters"] = created.every((c) => /^[0-9a-zA-Z]{7}$/.test(c.code));

const refused = {
    blocked: (await post({ url: "https://malware-ab.example/x" })).status,
    idnBlocked: (await post({ url: "https://phish-bänk.example/login" })).status,
    invalid: (await post({ url: "not a url" })).status,
    scheme: (await post({ url: "javascript:alert(1)" })).status,
    alias: (await post({ url: "https://a.example", alias: "Spring Sale!" })).status,
    aliasAgain: (await post({ url: "https://b.example", alias: "spring-sale-" })).status,
    notJson: (await post("{oops")).status,
};
results["blocklisted domain is refused"] = refused.blocked === 403;
results["IDN lookalike is matched in punycode"] = refused.idnBlocked === 403;
results["invalid URL and non-http schemes are 400"] =
    refused.invalid === 400 && refused.scheme === 400 && refused.notJson === 400;
results["custom alias is normalised and unique"] = refused.alias === 201 && refused.aliasAgain === 409;
results["Bloom hits were confirmed, not trusted"] = bloomConfirms >= 2;

const burst = await Promise.all(
    Array.from({ length: 260 }, (_, i) => post({ url: `https://spam.example/${i}` }, "greedy")),
);
const throttled = burst.filter((r) => r.status === 429).length;
results["rate limiter throttles a single greedy client"] =
    throttled >= 50 && burst.filter((r) => r.status === 201).length <= 210;

const temp = await (await post({ url: "https://flash.example/sale", ttlSeconds: 60 })).json();
results["temporary link redirects before expiry"] =
    (await fetch(base + temp.short, { redirect: "manual" })).status === 301;
now += 61_000;
results["expired link answers 410"] = (await fetch(base + temp.short, { redirect: "manual" })).status === 410;
results["unknown code answers 404"] = (await fetch(base + "/zzzzzzz", { redirect: "manual" })).status === 404;

const REFS = ["https://news.example/", "https://social.example/feed", "https://mail.example/", undefined];
const users = Array.from({ length: 3000 }, (_, i) => ({
    ip: `10.${(i >> 8) & 255}.${i & 255}.${i % 7}`,
    ua: `agent-${i % 11}`,
}));
let redirectsOk = 0;
for (let wave = 0; wave < 40; wave++) {
    const rs = await Promise.all(
        Array.from({ length: 250 }, () => {
            const link = created[Math.min(created.length - 1, Math.floor(rng.exponential(1 / 12)))];
            const u = users[rng.nextBounded(users.length)];
            const headers = { "x-forwarded-for": u.ip, "user-agent": u.ua };
            const ref = REFS[rng.nextBounded(REFS.length)];
            if (ref) headers.referer = ref;
            return fetch(base + link.short, { redirect: "manual", headers }).then(
                (r) => r.status === 301 && r.headers.get("location") === link.url,
            );
        }),
    );
    redirectsOk += rs.filter(Boolean).length;
}
results["10 000 redirects all answer 301 with the right Location"] = redirectsOk === 10_000;
results["LRU absorbs most reads"] = dbReads < 1500;

const top = created[0].code;
const stats = await (await fetch(`${base}/stats/${top}`)).json();
const exactU = exactVisitors.get(top).size;
results["unique visitors from HyperLogLog within 5%"] = Math.abs(stats.uniques - exactU) / exactU < 0.05;
const exactTop5 = exactClicks
    .entrySet()
    .sort((a, b) => b[1] - a[1])
    .slice(0, 5)
    .map(([k]) => k);
const sketchTop5 = topLinks
    .list()
    .sort((a, b) => b[1] - b[2] - (a[1] - a[2]))
    .slice(0, 5)
    .map(([k]) => k);
results["TopK finds the exact top-5 links"] = exactTop5.every((k) => sketchTop5.includes(k));
let worstOver = 0;
for (const [k, n] of exactReferrers.entrySet()) worstOver = Math.max(worstOver, referrers.count(k) - n);
results["count-min never undercounts and overcounts little"] =
    exactReferrers.entrySet().every(([k, n]) => referrers.count(k) >= n) && worstOver <= 10;

print(
    Table(
        [
            ["links", created.length.format()],
            ["redirects", redirectsOk.format()],
            ["SQLite reads (LRU misses)", dbReads.format()],
            ["top link uniques (HLL / exact)", `${stats.uniques} / ${exactU}`],
            ["Bloom false positives", `${bloomFp} of 20,000`],
            ["throttled creates", throttled.format()],
        ],
        { head: ["metric", "value"] },
    ),
);

app.close();
db.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
