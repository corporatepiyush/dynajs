// CDN edge — caching reverse proxies in front of an origin: HTTP cache semantics (max-age, s-maxage, no-store,
// private, stale-while-revalidate, Vary), conditional revalidation, request coalescing, an origin shield tier,
// surrogate-key purges, byte ranges, compression negotiation, signed URLs and consistent-hash routing.
//
// WHAT IT IS
//   origin       an App serving product pages, an API, user-specific pages, large downloads and versioned assets,
//                each with the Cache-Control, ETag, Vary and Surrogate-Key headers a real site would send
//   edges        three edge nodes and one shield node, each an App with its own LRU cache; a request is routed
//                to an edge by consistent hashing of its URL (so one object lives on one edge); edges fill from the
//                shield, the shield fills from the origin
//   caching      freshness from s-maxage > max-age; no-store and private responses are never stored; responses
//                vary by the headers named in Vary (Accept-Encoding normalised to gzip/identity); a stale object
//                within stale-while-revalidate is served immediately and refreshed in the background; an expired
//                object with an ETag is revalidated with If-None-Match and a 304 refreshes it without a body
//   coalescing   50 simultaneous misses for one URL produce ONE origin request (single flight)
//   purge        POST /purge with a surrogate key removes every object tagged with it on every node
//   ranges       Range: bytes=… on a cached 1 MiB file is answered 206 from cache with Content-Range
//   compression  responses are gzipped at the edge (by the App) for clients that accept it; the cache stores identity
//   signed URLs  /private/… needs ?expires=…&sig=HMAC; expired or tampered links are 403 before any lookup
//   routing      a native ConsistentHash ring (100 virtual nodes per edge); getN(url, 2) names a fallback, so when
//                an edge dies only its own third of the URL space moves
//   analytics    per-state latency percentiles (HIT vs MISS) from TDigests, the hottest URLs from a TopK, distinct URLs
//                per edge from HyperLogLogs; the Zipf traffic generator samples through a SortedMap of cumulative
//                weights (ceilKey) instead of a linear scan
//
// MODULES  dyna:http (App) · dyna:structures (LRU, SortedMap, ConsistentHash, TDigest, TopK, HyperLogLog)
//          · dyna:compress (gunzip) · dyna:crypto (HMAC) · dyna:hash (SHA256) · dyna:async (sleep) · dyna:cli (Table)
//          · dyna:random
//
// RUN      dynajs examples/net/cdn-edge.js     drives a day's worth of traffic through the edges in a few seconds

import { App } from "dyna:http";
import { LRU, SortedMap, ConsistentHash, TDigest, TopK, HyperLogLog } from "dyna:structures";
import { gunzip } from "dyna:compress";
import { HMACHex, TimingSafeEqual } from "dyna:crypto";
import { SHA256Hex } from "dyna:hash";
import { sleep } from "dyna:async";
import { Table } from "dyna:cli";
import { Random } from "dyna:random";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const rng = new Random(80);
const SIGNING_KEY = "edge-signing-key-2026";
const enc = new TextEncoder();

// =============================================================================
// Origin
// =============================================================================
const originStats = { requests: 0, byPath: new Map(), conditional304: 0 };
let priceVersion = 1;
const bigFile = new Random(1).bytes(1024 * 1024);
const origin = new App({ port: 0 });
origin.use((req) => {
    originStats.requests++;
    originStats.byPath.set(req.path, (originStats.byPath.get(req.path) || 0) + 1);
});
origin.get("/products/:id", async (req) => {
    await sleep(15);
    const body = `<html><body><h1>Product ${req.params.id}</h1><p>${"Lorem ipsum dolor sit amet. ".repeat(80)}</p><p>price v${priceVersion}</p></body></html>`;
    const etag = `"${SHA256Hex(body).slice(0, 16)}"`;
    if (req.headers["if-none-match"] === etag) {
        originStats.conditional304++;
        return {
            status: 304,
            body: "",
            headers: { etag, "cache-control": "public, max-age=60, s-maxage=1, stale-while-revalidate=30" },
        };
    }
    return {
        body,
        contentType: "text/html",
        headers: {
            etag,
            "cache-control": "public, max-age=60, s-maxage=1, stale-while-revalidate=30",
            "surrogate-key": `product-${req.params.id} products`,
            vary: "Accept-Encoding",
        },
    };
});
origin.get("/api/stock/:id", async (req) => {
    await sleep(10);
    return {
        body: JSON.stringify({ id: req.params.id, stock: rng.nextBounded(50) }),
        contentType: "application/json",
        headers: { "cache-control": "public, max-age=2", "surrogate-key": `product-${req.params.id}` },
    };
});
origin.get("/account", (req) => ({
    body: `<p>Hello ${req.headers.cookie || "guest"}</p>`,
    contentType: "text/html",
    headers: { "cache-control": "private, no-store" },
}));
origin.get("/assets/:file", (req) => ({
    body: `/* ${req.params.file} */ ${"body{margin:0} ".repeat(200)}`,
    contentType: "text/css",
    headers: { "cache-control": "public, max-age=31536000, immutable", "surrogate-key": "assets" },
}));
origin.get("/downloads/big.bin", () => ({
    body: bigFile,
    contentType: "application/octet-stream",
    headers: { "cache-control": "public, max-age=3600", etag: '"big-v1"' },
}));
origin.get("/private/:file", (req) => ({
    body: `secret report ${req.params.file}`,
    contentType: "text/plain",
    headers: { "cache-control": "public, max-age=600" },
}));
origin.start();

// =============================================================================
// Cache node (edge or shield)
// =============================================================================
const parseCC = (h = "") =>
    Object.fromEntries(
        h
            .split(",")
            .map((p) => p.trim())
            .filter(Boolean)
            .map((p) => {
                const [k, v] = p.split("=");
                return [k.toLowerCase(), v === undefined ? true : Number(v)];
            }),
    );
const normEncoding = (h = "") => (/\bgzip\b/.test(h) ? "gzip" : "identity");
const COMPRESSIBLE = /^(text\/|application\/(json|javascript))/;
class CacheNode {
    constructor(name, upstream, { verifySignatures = false } = {}) {
        this.name = name;
        this.upstream = upstream;
        this.verifySignatures = verifySignatures;
        this.cache = new LRU(500);
        this.tags = new Map();
        this.inflight = new Map();
        this.stats = { hits: 0, misses: 0, stale: 0, revalidated: 0, coalesced: 0, bypass: 0 };
        this.app = new App({ port: 0 });
        this.app.post("/purge", (req) => ({ purged: this.purge(JSON.parse(req.body).key) }));
        this.app.get("/*path", (req) => this.serve(req));
        this.app.start();
    }
    key(path, req, vary) {
        return `${path}|${(vary || []).map((h) => `${h}=${h === "accept-encoding" ? normEncoding(req.headers[h]) : req.headers[h] || ""}`).join("&")}`;
    }
    purge(tag) {
        const keys = this.tags.get(tag) || new Set();
        for (const k of keys) this.cache.delete(k);
        this.tags.delete(tag);
        return keys.size;
    }
    async fetchUpstream(url, req, cached) {
        const headers = { "accept-encoding": "identity" };
        if (cached?.etag) headers["if-none-match"] = cached.etag;
        if (req.headers.cookie) headers.cookie = req.headers.cookie;
        const r = await fetch(this.upstream + url, { headers });
        const body = new Uint8Array(await r.arrayBuffer());
        const h = Object.fromEntries([...r.headers].map(([k, v]) => [k.toLowerCase(), v]));
        return { status: r.status, body, headers: h };
    }
    store(key, res, now) {
        const cc = parseCC(res.headers["cache-control"]);
        if (cc["no-store"] || cc.private || res.status !== 200) return null;
        const ttl = (cc["s-maxage"] ?? cc["max-age"] ?? 0) * 1000;
        const entry = {
            status: 200,
            body: res.body,
            headers: res.headers,
            etag: res.headers.etag,
            storedAt: now,
            freshUntil: now + ttl,
            swr: (cc["stale-while-revalidate"] || 0) * 1000,
            vary: (res.headers.vary || "")
                .toLowerCase()
                .split(",")
                .map((s) => s.trim())
                .filter(Boolean),
        };
        this.cache.put(key, entry);
        return entry;
    }
    async lookup(path, req) {
        const now = Date.now(),
            varyKey = `vary|${path}`,
            vary = this.cache.get(varyKey) || [];
        const key = this.key(path, req, vary);
        const hit = this.cache.get(key);
        if (hit && now < hit.freshUntil) {
            this.stats.hits++;
            return { entry: hit, state: "HIT" };
        }
        if (hit && now < hit.freshUntil + hit.swr) {
            // serve stale, refresh in the background
            this.stats.stale++;
            if (!this.inflight.has(key))
                this.inflight.set(
                    key,
                    this.refill(path, req, key, hit).finally(() => this.inflight.delete(key)),
                );
            return { entry: hit, state: "STALE" };
        }
        if (this.inflight.has(key)) {
            this.stats.coalesced++;
            return { entry: await this.inflight.get(key), state: "COALESCED" };
        }
        this.stats.misses++;
        const p = this.refill(path, req, key, hit).finally(() => this.inflight.delete(key));
        this.inflight.set(key, p);
        return { entry: await p, state: hit ? "REVALIDATED" : "MISS" };
    }
    async refill(path, req, key, stale) {
        const res = await this.fetchUpstream(path, req, stale);
        if (res.status === 304 && stale) {
            this.stats.revalidated++;
            const cc = parseCC(res.headers["cache-control"]);
            stale.freshUntil = Date.now() + (cc["s-maxage"] ?? cc["max-age"] ?? 0) * 1000;
            this.cache.put(key, stale);
            return stale;
        }
        const entry = this.store(key, res, Date.now());
        let finalKey = key;
        if (entry && entry.vary.length) {
            this.cache.put(`vary|${path}`, entry.vary);
            finalKey = this.key(path, req, entry.vary);
            if (finalKey !== key) {
                this.cache.put(finalKey, entry);
                this.cache.delete(key);
            }
        }
        if (entry)
            for (const tag of (res.headers["surrogate-key"] || "").split(/\s+/).filter(Boolean)) {
                if (!this.tags.has(tag)) this.tags.set(tag, new Set());
                this.tags.get(tag).add(finalKey);
            }
        if (!entry) this.stats.bypass++;
        return entry || { status: res.status, body: res.body, headers: res.headers, uncacheable: true };
    }
    async serve(req) {
        if (this.verifySignatures && req.path.startsWith("/private/")) {
            const exp = Number(req.query.expires),
                sig = req.query.sig || "",
                want = HMACHex("sha256", SIGNING_KEY, `${req.path}|${exp}`);
            if (
                !(exp > Date.now() / 1000) ||
                sig.length !== want.length ||
                !TimingSafeEqual(enc.encode(sig), enc.encode(want))
            )
                return { status: 403, body: "invalid or expired signature" };
        }
        const cacheable = !req.headers.cookie || !req.path.startsWith("/account");
        const { entry, state } = cacheable
            ? await this.lookup(req.path, req)
            : { entry: await this.refill(req.path, req, null, null), state: "PASS" };
        let body = entry.body,
            status = entry.status;
        const headers = { "x-cache": `${state} from ${this.name}` },
            contentType = entry.headers["content-type"] || "application/octet-stream";
        if (entry.etag) headers.etag = entry.etag;
        for (const h of ["cache-control", "surrogate-key"]) if (entry.headers?.[h]) headers[h] = entry.headers[h]; // the next tier caches by the same rules
        if (entry.storedAt) headers.age = String(Math.floor((Date.now() - entry.storedAt) / 1000));
        const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || "");
        if (range && status === 200) {
            const start = Number(range[1]),
                end = range[2] ? Math.min(Number(range[2]), body.length - 1) : body.length - 1;
            if (start >= body.length)
                return { status: 416, body: "", headers: { "content-range": `bytes */${body.length}` } };
            headers["content-range"] = `bytes ${start}-${end}/${body.length}`;
            body = body.subarray(start, end + 1);
            status = 206;
        } else if (COMPRESSIBLE.test(contentType)) headers.vary = "Accept-Encoding"; // the App itself gzips for clients that accept it
        return { status, body, headers, contentType };
    }
}

// =============================================================================
// Topology: shield + three edges, consistent-hash routing
// =============================================================================
const shield = new CacheNode("shield", `http://127.0.0.1:${origin.port}`);
const edges = ["fra", "ams", "lhr"].map(
    (n) => new CacheNode(`edge-${n}`, `http://127.0.0.1:${shield.app.port}`, { verifySignatures: true }),
);
const ring = new ConsistentHash({ replicas: 100 });
const byName = new Map(edges.map((e) => [e.name, e]));
for (const e of edges) ring.addNode(e.name);
const down = new Set();
const route = (path) => byName.get(ring.getN(path.split("?")[0], 2).find((n) => !down.has(n)));
const latency = new Map(),
    hot = new TopK(10),
    distinct = new Map(edges.map((e) => [e.name, new HyperLogLog(12)]));
const get = async (path, headers = {}) => {
    const edge = route(path),
        t0 = performance.now();
    const r = await fetch(`http://127.0.0.1:${edge.app.port}${path}`, { headers });
    const body = new Uint8Array(await r.arrayBuffer());
    const h = Object.fromEntries([...r.headers].map(([k, v]) => [k.toLowerCase(), v]));
    const state = (h["x-cache"] || "NONE").split(" ")[0];
    latency.getOrInsertComputed(state, () => new TDigest(100)).add(performance.now() - t0);
    if (path.startsWith("/products/")) hot.add(path);
    distinct.get(edge.name).add(path.split("?")[0]);
    return { status: r.status, body, headers: h, edge: edge.name };
};
const sign = (path, ttl) => {
    const exp = Math.floor(Date.now() / 1000) + ttl;
    return `${path}?expires=${exp}&sig=${HMACHex("sha256", SIGNING_KEY, `${path}|${exp}`)}`;
};

// =============================================================================
// Traffic and drills
// =============================================================================
const results = {};
const states = new Map();
const tally = (r) => {
    const s = r.headers["x-cache"].split(" ")[0];
    states.set(s, (states.get(s) || 0) + 1);
    return r;
};

// Coalescing: 50 concurrent requests for a cold object.
originStats.requests = 0;
const burst = await Promise.all(
    Array.from({ length: 50 }, () => get("/products/1", { "accept-encoding": "gzip" }).then(tally)),
);
results[`50 concurrent misses became ${originStats.byPath.get("/products/1")} origin request`] =
    burst.every((r) => r.status === 200) && originStats.byPath.get("/products/1") === 1;
const decoded = (r) => new TextDecoder().decode(r.body[0] === 0x1f && r.body[1] === 0x8b ? gunzip(r.body) : r.body);
results["gzip is negotiated at the edge"] =
    burst.every((r) => r.headers["content-encoding"] === "gzip") && decoded(burst[0]).includes("Product 1");
const plain = await get("/products/1");
results["a client without gzip gets identity from the same cache entry"] =
    !plain.headers["content-encoding"] && new TextDecoder().decode(plain.body).includes("Product 1");

// Zipf-distributed browsing.
const paths = Array.from({ length: 200 }, (_, i) => `/products/${i}`);
const zipf = new SortedMap();
let cumulative = 0;
paths.forEach((_, i) => zipf.set((cumulative += 1 / (i + 1)), i)); // cumulative weight -> path index
const pickPath = () => paths[zipf.get(zipf.ceilKey(rng.nextFloat() * cumulative))];
const before = originStats.requests;
for (let i = 0; i < 1500; i++) tally(await get(pickPath(), { "accept-encoding": i % 3 ? "gzip, br" : "" }));
for (let i = 0; i < 300; i++) tally(await get(`/assets/app-${i % 5}.css`, { "accept-encoding": "gzip" }));
const browsing = originStats.requests - before;
results[
    `origin saw ${browsing} requests for 1800 page and asset views (hit ratio ${(1 - browsing / 1800).toFixed(2)})`
] = browsing < 1800 * 0.25;

// Freshness: s-maxage=1 expires quickly; within stale-while-revalidate the stale copy is served and refreshed; the ETag gives 304s.
await sleep(1200);
const stale = await get("/products/1");
await sleep(50);
results["stale-while-revalidate served immediately and revalidated with If-None-Match (304)"] =
    stale.headers["x-cache"].startsWith("STALE") &&
    shield.stats.revalidated + edges.reduce((s, e) => s + e.stats.revalidated, 0) >= 1 &&
    originStats.conditional304 >= 1;

// No-store and private.
const a1 = await get("/account", { cookie: "user=alice" }),
    a2 = await get("/account", { cookie: "user=bob" });
results["private, no-store pages are never cached or mixed between users"] =
    new TextDecoder().decode(a1.body).includes("alice") &&
    new TextDecoder().decode(a2.body).includes("bob") &&
    [shield, ...edges].every((e) => !e.cache.has("/account|")) &&
    route("/account").stats.bypass >= 2;

// Purge by surrogate key: a price change on product 3.
await get("/products/3");
priceVersion = 2;
const stillOld = await get("/products/3");
const purged = await Promise.all(
    [shield, ...edges].map(
        async (n) =>
            (
                await (
                    await fetch(`http://127.0.0.1:${n.app.port}/purge`, {
                        method: "POST",
                        body: JSON.stringify({ key: "product-3" }),
                    })
                ).json()
            ).purged,
    ),
);
const fresh = await get("/products/3");
results[
    `purging surrogate key product-3 removed ${purged.reduce((a, b) => a + b, 0)} objects; the next request sees the new price`
] =
    new TextDecoder().decode(stillOld.body).includes("price v1") &&
    new TextDecoder().decode(fresh.body).includes("price v2") &&
    purged.reduce((a, b) => a + b, 0) >= 2;

// Ranges on a cached download.
await get("/downloads/big.bin");
const originBig = originStats.byPath.get("/downloads/big.bin");
const part = await get("/downloads/big.bin", { range: "bytes=1000-1999" }),
    tail = await get("/downloads/big.bin", { range: "bytes=1048000-" }),
    beyond = await get("/downloads/big.bin", { range: "bytes=9999999-" });
results["a byte range is served 206 from cache"] =
    part.status === 206 &&
    part.body.length === 1000 &&
    part.body[0] === bigFile[1000] &&
    part.headers["content-range"] === "bytes 1000-1999/1048576";
results["open-ended and unsatisfiable ranges, with no extra origin traffic"] =
    tail.body.length === 1048576 - 1048000 &&
    beyond.status === 416 &&
    originStats.byPath.get("/downloads/big.bin") === originBig;

// Signed URLs.
const okUrl = sign("/private/q2-report.pdf", 60);
const ok = await get(okUrl),
    tampered = await get(okUrl.replace("q2", "q3")),
    expired = await get(sign("/private/q2-report.pdf", -5));
results["signed URLs: valid 200, tampered 403, expired 403"] =
    ok.status === 200 && tampered.status === 403 && expired.status === 403;

// Routing: each product lives on exactly one edge.
const homes = new Map();
for (const p of paths) homes.set(p, route(p).name);
results["consistent hashing spreads URLs over all edges and is stable"] =
    new Set(homes.values()).size === 3 && [...homes].every(([p, e]) => route(p).name === e);
results["latency: cache hits are faster than misses at the median"] =
    latency.get("HIT").quantile(0.5) < latency.get("MISS").quantile(0.5);
results[
    `the TopK's hottest product pages are the Zipf head (${hot
        .list()
        .take(3)
        .map(([p]) => p)
        .join(" ")})`
] =
    hot
        .list()
        .take(3)
        .some(([p]) => p === "/products/0") &&
    hot
        .list()
        .take(3)
        .some(([p]) => p === "/products/1");
// Edge failure: take one edge out; only its URLs move, to their second choice, and they still load.
const victim = edges[1].name;
down.add(victim);
const moved = paths.filter((p) => route(p).name !== homes.get(p));
const afterFail = await Promise.all(moved.take(10).map((p) => get(p)));
results[`losing ${victim} moves only its own ${moved.length} URLs, which keep loading`] =
    moved.length > 0 &&
    moved.all((p) => homes.get(p) === victim) &&
    afterFail.all((r) => r.status === 200 && r.edge !== victim);
const distinctTotal = [...distinct.values()].reduce((s, h) => s + h.count(), 0);

print("cdn-edge");
print(
    table([
        ["node", "hits", "misses", "stale", "revalidated", "coalesced", "bypass", "tags"],
        ...[shield, ...edges].map((n) => [
            n.name,
            ...["hits", "misses", "stale", "revalidated", "coalesced", "bypass"].map((k) => String(n.stats[k])),
            String(n.tags.size),
        ]),
    ]),
);
print(
    `  responses  ${[...states].map(([k, v]) => `${k} ${v}`).join(", ")}; origin requests ${originStats.requests}, of which ${originStats.conditional304} were 304s`,
);
print(
    `  latency    ${[...latency].map(([k, d]) => `${k} p50 ${d.quantile(0.5).round(2)} ms / p99 ${d.quantile(0.99).round(2)} ms`).join(" | ")}`,
);
print(
    `  urls       ~${distinctTotal.round(0)} distinct URL x edge pairs; hottest products ${hot
        .list()
        .take(3)
        .map(([p, n]) => `${p} ${n}`)
        .join(", ")}`,
);
await Promise.allSettled([shield, ...edges].flatMap((n) => [...n.inflight.values()])); // background refreshes finish first
for (const n of [shield, ...edges]) n.app.close();
origin.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
