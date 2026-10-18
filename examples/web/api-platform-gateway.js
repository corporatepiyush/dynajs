// API platform gateway — config-driven routing with breakers, retries, bulkheads, caching, auth, quotas and tracing.
//
// WHAT IT IS
//   The edge tier of an API platform, configured from YAML and reloadable without a restart:
//
//   config        routes, upstream replicas with weights, tenants (key digests, networks, quotas) in one
//                 YAML document; a reload validates the new document against a JSON Schema first and keeps
//                 the old one if it is wrong
//   auth          API keys checked as HMAC-SHA256 digests (the table never holds a usable key); tenant
//                 networks as compiled CIDR prefixes
//   quotas        a token bucket per tenant; 429 with Retry-After
//   routing       weighted choice among an upstream's healthy replicas
//   resilience    a circuit breaker per replica (closed -> open after consecutive failures -> half-open
//                 probe after a cool-down), retries with backoff for idempotent methods only, and a
//                 per-route bulkhead (a semaphore) so one slow backend cannot take every connection
//   caching       GET responses cached per tenant+URL in an LRU, revalidated upstream with If-None-Match
//   tracing       a UUIDv7 trace id on every request, forwarded upstream and stamped on every log line
//   metrics       counters and latency histograms in Prometheus format; per-route latency TDigests give true
//                 p50/p99 on /admin/status, and per-tenant TopKs of request paths back /admin/usage
//   routing index the route prefixes are compiled into a Trie on every (re)load, so choosing a route is one
//                 longestPrefix() lookup rather than a scan of the sorted route list
//
// MODULES  dyna:http · dyna:net (RateLimiter, Prefix, Metrics) · dyna:crypto · dyna:yaml · dyna:schema
//          dyna:async (retry, Semaphore) · dyna:structures (LRU, Trie, TDigest, TopK) · dyna:log · dyna:uuid
//          · dyna:random · dyna:file
//
// RUN      dynajs examples/web/api-platform-gateway.js --config gateway.yaml --port 8443
//          dynajs examples/web/api-platform-gateway.js        self-test: three upstream replicas (one flaky,
//          one that goes down), traffic from two tenants, a breaker trip and recovery, cache revalidation,
//          a bad config reload refused and a good one applied

import { App } from "dyna:http";
import { RateLimiter, Prefix, Metrics } from "dyna:net";
import { Hmac, RandomBytes } from "dyna:crypto";
import { Parse as parseYAML, Stringify as toYAML } from "dyna:yaml";
import { Schema } from "dyna:schema";
import { retry, Semaphore, sleep } from "dyna:async";
import { LRU, Trie, TDigest, TopK } from "dyna:structures";
import { Logger } from "dyna:log";
import { v7 } from "dyna:uuid";
import { Random } from "dyna:random";
import { Path, makeTempDir, writeFile, readFile, removeAll } from "dyna:file";
import { Command } from "dyna:cli";

const { options } = new Command("api-platform-gateway")
    .option("-c, --config <file>", "gateway YAML", { type: "string" })
    .option("-p, --port <n>", "listen port", { type: "number", default: 0 })
    .option("--pepper <hex>", "HMAC pepper for key digests (hex)", { type: "string", env: "GATEWAY_PEPPER" })
    .parse(scriptArgs.slice(1));
const selfTest = !options.config;

// =============================================================================
// Configuration
// =============================================================================
const configSchema = Schema.compile({
    type: "object",
    required: ["upstreams", "routes", "tenants"],
    additionalProperties: false,
    properties: {
        upstreams: {
            type: "object",
            minProperties: 1,
            additionalProperties: {
                type: "array",
                minItems: 1,
                items: {
                    type: "object",
                    required: ["url"],
                    additionalProperties: false,
                    properties: {
                        url: { type: "string", pattern: "^https?://" },
                        weight: { type: "integer", minimum: 1, maximum: 100 },
                    },
                },
            },
        },
        routes: {
            type: "array",
            minItems: 1,
            items: {
                type: "object",
                required: ["prefix", "upstream"],
                additionalProperties: false,
                properties: {
                    prefix: { type: "string", pattern: "^/" },
                    upstream: { type: "string" },
                    maxConcurrent: { type: "integer", minimum: 1 },
                    timeoutMs: { type: "integer", minimum: 50 },
                    cacheSeconds: { type: "integer", minimum: 0 },
                },
            },
        },
        tenants: {
            type: "object",
            additionalProperties: {
                type: "object",
                required: ["keyDigest", "networks", "perSecond"],
                additionalProperties: false,
                properties: {
                    keyDigest: { type: "string", pattern: "^[0-9a-f]{64}$" },
                    networks: { type: "array", items: { type: "string" } },
                    perSecond: { type: "number", exclusiveMinimum: 0 },
                },
            },
        },
    },
});

class Breaker {
    // closed -> open -> half-open -> closed
    constructor(threshold = 3, coolDownMs = 300) {
        this.threshold = threshold;
        this.coolDownMs = coolDownMs;
        this.failures = 0;
        this.openedAt = 0;
        this.state = "closed";
    }
    available() {
        if (this.state === "open" && performance.now() - this.openedAt >= this.coolDownMs) this.state = "half-open";
        return this.state !== "open";
    }
    success() {
        this.failures = 0;
        this.state = "closed";
    }
    failure() {
        this.failures++;
        if (this.state === "half-open" || this.failures >= this.threshold) {
            this.state = "open";
            this.openedAt = performance.now();
        }
    }
}

let live = null; // the compiled configuration in force
function compile(doc) {
    const v = configSchema.validate(doc);
    if (!v.valid) throw new Error("invalid config: " + v.errors.map((e) => `${e.path} ${e.message}`).join("; "));
    for (const r of doc.routes)
        if (!doc.upstreams[r.upstream]) throw new Error(`route ${r.prefix} names unknown upstream ${r.upstream}`);
    const upstreams = new Map(
        Object.entries(doc.upstreams).map(([name, reps]) => [
            name,
            reps.map((r) => ({
                url: r.url,
                weight: r.weight || 1,
                breaker: live?.upstreams.get(name)?.find((o) => o.url === r.url)?.breaker || new Breaker(),
            })),
        ]),
    );
    const tenants = new Map(
        Object.entries(doc.tenants).map(([name, t]) => [
            t.keyDigest,
            {
                name,
                networks: t.networks.map((c) => new Prefix(c)),
                limiter: new RateLimiter({ tokensPerSec: t.perSecond, burst: Math.max(1, Math.ceil(t.perSecond * 2)) }),
            },
        ]),
    );
    const routes = doc.routes
        .map((r) => ({
            ...r,
            bulkhead: new Semaphore(r.maxConcurrent || 64),
            timeoutMs: r.timeoutMs || 2000,
            cacheSeconds: r.cacheSeconds || 0,
        }))
        .sort((a, b) => b.prefix.length - a.prefix.length); // longest prefix wins
    const routeTrie = new Trie(),
        byPrefix = new Map(routes.map((r) => [r.prefix, r]));
    for (const r of routes) routeTrie.insert(r.prefix);
    return {
        upstreams,
        tenants,
        routes,
        routeFor: (path) => byPrefix.get(routeTrie.longestPrefix(path)),
        loadedAt: new Date().toISOString(),
    };
}

// =============================================================================
// The gateway
// =============================================================================
const pepper = options.pepper ? Uint8Array.from(options.pepper.match(/../g), (h) => parseInt(h, 16)) : RandomBytes(32);
const digester = new Hmac("sha256", pepper);
const rng = new Random(7);
const cache = new LRU(5000);
const routeLatency = new Map(),
    tenantPaths = new Map(); // route -> TDigest(ms), tenant -> TopK(paths)
const logLines = [];
const log = new Logger({
    name: "gateway",
    timestamp: false,
    dest: selfTest ? (l) => logLines.push(JSON.parse(l)) : undefined,
});

function pickReplica(name) {
    const healthy = live.upstreams.get(name).filter((r) => r.breaker.available());
    if (!healthy.length) return null;
    let roll = rng.nextBounded(healthy.reduce((s, r) => s + r.weight, 0));
    return healthy.find((r) => (roll -= r.weight) < 0);
}
async function callUpstream(route, req, trace, path) {
    const idempotent = req.method === "GET" || req.method === "PUT" || req.method === "DELETE";
    const cacheKey = `${req.tenant.name} ${path}`;
    const cached = req.method === "GET" && route.cacheSeconds ? cache.get(cacheKey) : undefined;
    let attempt = 0;
    const once = async () => {
        attempt++;
        const replica = pickReplica(route.upstream);
        if (!replica) throw Object.assign(new Error("no healthy replica"), { status: 503, final: true });
        const headers = {
            "x-trace-id": trace,
            "x-tenant": req.tenant.name,
            "content-type": req.headers["content-type"] || "application/json",
        };
        if (cached) headers["if-none-match"] = cached.etag;
        let r;
        try {
            r = await fetch(replica.url + path, {
                method: req.method,
                headers,
                body: req.method === "GET" ? undefined : req.body,
                timeout: route.timeoutMs,
            });
        } catch (e) {
            replica.breaker.failure();
            throw Object.assign(e, { status: 502 });
        }
        const text = await r.text();
        if (r.status >= 500) {
            replica.breaker.failure();
            throw Object.assign(new Error(`upstream ${r.status}`), { status: r.status });
        }
        replica.breaker.success();
        log.info(
            { trace, tenant: req.tenant.name, replica: replica.url, status: r.status, attempt },
            "upstream answered",
        );
        if (r.status === 304 && cached)
            return { ...cached.response, headers: { ...cached.response.headers, "x-cache": "revalidated" } };
        const response = {
            status: r.status,
            body: text,
            contentType: r.headers.get("content-type") || "application/json",
            headers: { "x-trace-id": trace, "x-cache": "miss" },
        };
        if (req.method === "GET" && route.cacheSeconds && r.headers.get("etag"))
            cache.setWithTTL(cacheKey, { etag: r.headers.get("etag"), response }, route.cacheSeconds * 1000);
        return response;
    };
    if (!idempotent) return once();
    return retry(once, {
        retries: 2,
        backoffMs: 20,
        factor: 2,
        onRetry: (e) => {
            if (e.final) throw e;
        },
    });
}

const app = new App({ port: options.port });
app.get("/metrics", () => ({ body: Metrics.scrape(), contentType: "text/plain" }));
app.get("/admin/usage", () =>
    Object.fromEntries(
        [...tenantPaths].map(([t, k]) => [
            t,
            k
                .list()
                .take(3)
                .map(([path, n, err]) => ({ path, requests: n - err })),
        ]),
    ),
);
app.get("/admin/status", () => ({
    loadedAt: live.loadedAt,
    latency: Object.fromEntries(
        [...routeLatency].map(([r, d]) => [r, { p50: d.quantile(0.5), p99: d.quantile(0.99) }]),
    ),
    upstreams: Object.fromEntries(
        [...live.upstreams].map(([n, rs]) => [n, rs.map((r) => ({ url: r.url, breaker: r.breaker.state }))]),
    ),
}));
app.post("/admin/reload", () => {
    try {
        live = compile(parseYAML(readFile(configPath)));
        log.info({ event: "config.reloaded" }, "configuration reloaded");
        return { ok: true, loadedAt: live.loadedAt };
    } catch (e) {
        log.warn({ event: "config.rejected", reason: e.message }, "configuration rejected; keeping the previous one");
        return { status: 422, body: JSON.stringify({ ok: false, error: e.message }), contentType: "application/json" };
    }
});

app.use((req) => {
    if (req.path.startsWith("/admin/") || req.path === "/metrics") return;
    req.trace = req.headers["x-trace-id"] || v7();
    req.t0 = performance.now();
    const deny = (status, error, headers = {}) => {
        Metrics.counter("gateway_responses_total", 1, { tenant: req.tenant?.name || "-", status: String(status) });
        log.info({ trace: req.trace, status, error }, "denied");
        return {
            response: {
                status,
                body: JSON.stringify({ error, trace: req.trace }),
                contentType: "application/json",
                headers: { "x-trace-id": req.trace, ...headers },
            },
        };
    };
    const key = req.headers["x-api-key"];
    req.tenant = key ? live.tenants.get(digester.signHex(key)) : undefined;
    if (!req.tenant) return deny(401, "unknown API key");
    const addr = (req.headers["x-forwarded-for"] || "").split(",")[0].trim();
    if (!req.tenant.networks.some((p) => p.contains(addr))) return deny(403, "network not allowed");
    if (!req.tenant.limiter.allow(req.tenant.name)) return deny(429, "quota exceeded", { "retry-after": "1" });
});
async function proxy(req) {
    const route = live.routeFor(req.path);
    tenantPaths.getOrInsertComputed(req.tenant.name, () => new TopK(32)).add(req.path);
    if (!route) return { status: 404, body: JSON.stringify({ error: "no route" }), contentType: "application/json" };
    const release = await route.bulkhead.acquire();
    try {
        const path = req.path + (Object.keys(req.query).length ? "?" + new URLSearchParams(req.query) : "");
        const r = await callUpstream(route, req, req.trace, path);
        Metrics.counter("gateway_responses_total", 1, { tenant: req.tenant.name, status: String(r.status) });
        Metrics.histogram("gateway_seconds", (performance.now() - req.t0) / 1000, { route: route.prefix });
        routeLatency.getOrInsertComputed(route.prefix, () => new TDigest(100)).add(performance.now() - req.t0);
        return r;
    } catch (e) {
        Metrics.counter("gateway_responses_total", 1, { tenant: req.tenant.name, status: String(e.status || 502) });
        log.error({ trace: req.trace, error: e.message }, "upstream failed");
        return {
            status: e.status === 503 ? 503 : 502,
            body: JSON.stringify({ error: e.message, trace: req.trace }),
            contentType: "application/json",
            headers: { "x-trace-id": req.trace },
        };
    } finally {
        release();
    }
}
for (const m of ["get", "post", "put", "del"]) app[m]("/*path", proxy);

// =============================================================================
// Boot
// =============================================================================
let configPath,
    work,
    upstreamApps = [],
    keys = {},
    demo = null;
if (selfTest) ({ configPath, work, upstreamApps, keys, demo } = await startDemoUpstreams());
else configPath = new Path(options.config);
live = compile(parseYAML(readFile(configPath)));
app.start();
if (!selfTest) print(`gateway on :${app.port} with ${live.routes.length} routes, ${live.tenants.size} tenants`);
else await selfCheck();

// Demo environment: three replicas of an orders service, one inventory service.
async function startDemoUpstreams() {
    const work = makeTempDir("gateway");
    const state = { flaky: 0, down: false, orderHits: 0 };
    const mk = (name, handler) => {
        const a = new App({ port: 0 });
        handler(a);
        a.start();
        return a;
    };
    const orders = (label, opts = {}) =>
        mk(label, (a) => {
            a.get("/orders/*rest", (req) => {
                state.orderHits++;
                if (opts.flaky && state.flaky++ % 2 === 0) return { status: 500, body: "transient" };
                if (opts.downable && state.down) return { status: 503, body: "down" };
                return {
                    status: 200,
                    body: JSON.stringify({
                        served: label,
                        tenant: req.headers["x-tenant"],
                        trace: req.headers["x-trace-id"],
                        path: req.path,
                    }),
                    contentType: "application/json",
                };
            });
        });
    const catalogETag = '"v42"';
    const inventory = mk("inventory", (a) =>
        a.get("/inventory/*rest", (req) =>
            req.headers["if-none-match"] === catalogETag
                ? { status: 304, body: "", headers: { etag: catalogETag } }
                : {
                      status: 200,
                      body: JSON.stringify({ sku: req.params.rest, stock: 12 }),
                      contentType: "application/json",
                      headers: { etag: catalogETag },
                  },
        ),
    );
    const apps = [
        orders("orders-a"),
        orders("orders-b", { flaky: true }),
        orders("orders-c", { downable: true }),
        inventory,
    ];
    const k = { acme: "gk_acme_demo1234", globex: "gk_globex_demo5678" }; // issued to the tenants out of band
    const doc = {
        upstreams: {
            orders: apps.slice(0, 3).map((a, i) => ({ url: `http://127.0.0.1:${a.port}`, weight: [5, 3, 2][i] })),
            inventory: [{ url: `http://127.0.0.1:${apps[3].port}` }],
        },
        routes: [
            { prefix: "/orders/", upstream: "orders", maxConcurrent: 4, timeoutMs: 1000 },
            { prefix: "/inventory/", upstream: "inventory", cacheSeconds: 60 },
        ],
        tenants: {
            acme: { keyDigest: digester.signHex(k.acme), networks: ["10.0.0.0/8"], perSecond: 200 },
            globex: { keyDigest: digester.signHex(k.globex), networks: ["192.0.2.0/24"], perSecond: 2 },
        },
    };
    const path = work.join("gateway.yaml");
    writeFile(path, toYAML(doc));
    return { configPath: path, work, upstreamApps: apps, keys: k, demo: state };
}

async function selfCheck() {
    const results = {};
    const GW = `http://127.0.0.1:${app.port}`;
    const call = async (path, key, from, extra = {}) => {
        const r = await fetch(GW + path, { headers: { "x-api-key": key, "x-forwarded-for": from, ...extra } });
        const text = await r.text();
        return {
            status: r.status,
            cache: r.headers.get("x-cache"),
            trace: r.headers.get("x-trace-id"),
            body: text.startsWith("{") ? JSON.parse(text) : text,
        };
    };
    const ACME = [keys.acme, "10.1.2.3"];

    // Weighted routing with a flaky replica: retries hide the transient 500s from the caller.
    const served = {};
    for (let i = 0; i < 60; i++) {
        const r = await call(`/orders/${i}`, ...ACME);
        results[`order ${i} -> ${r.status}`] = r.status === 200;
        served[r.body.served] = (served[r.body.served] || 0) + 1;
    }
    results[`weights respected ${JSON.stringify(served)}`] = served["orders-a"] > served["orders-c"];
    const traced = await call("/orders/trace-me", ...ACME, { "x-trace-id": "trace-from-client" });
    results["a client trace id is propagated upstream and back"] =
        traced.body.trace === "trace-from-client" && traced.trace === "trace-from-client";

    // Replica C goes down: its breaker opens, traffic flows to the others, and status shows it.
    demo.down = true;
    const during = [];
    for (let i = 0; i < 30; i++) during.push((await call(`/orders/x${i}`, ...ACME)).status);
    results["callers keep succeeding while a replica fails"] = during.all((st) => st === 200);
    const status = await (await fetch(GW + "/admin/status")).json();
    results["the failing replica's breaker is open"] =
        status.upstreams.orders.find((r) => r.url.endsWith(String(upstreamApps[2].port))).breaker === "open";
    demo.down = false;
    await sleep(350); // cool-down elapses
    for (let i = 0; i < 40; i++) await call(`/orders/y${i}`, ...ACME);
    const after = await (await fetch(GW + "/admin/status")).json();
    results["the recovered replica is probed and closed again"] = after.upstreams.orders.every(
        (r) => r.breaker === "closed",
    );

    // Cache: first GET misses, the second revalidates upstream with If-None-Match and is served from cache.
    const c1 = await call("/inventory/SKU-1", ...ACME),
        c2 = await call("/inventory/SKU-1", ...ACME);
    results[`cache ${c1.cache} -> ${c2.cache}`] =
        c1.cache === "miss" && c2.cache === "revalidated" && c2.body.stock === 12;

    // Auth, networks and quotas.
    results["unknown key"] = (await call("/orders/1", "gk_nope", "10.1.2.3")).status === 401;
    results["foreign network"] = (await call("/orders/1", keys.acme, "203.0.113.5")).status === 403;
    const burst = [];
    for (let i = 0; i < 6; i++) burst.push((await call("/orders/1", keys.globex, "192.0.2.10")).status);
    results[`globex quota enforced ${burst}`] = burst.filter((s) => s === 429).length >= 2;

    // Config reload: a broken document is refused and the old config keeps serving; a good one is applied.
    const doc = parseYAML(readFile(configPath));
    writeFile(configPath, toYAML({ ...doc, routes: [{ prefix: "orders", upstream: "nowhere" }] }));
    const bad = await fetch(GW + "/admin/reload", { method: "POST" });
    results["a bad reload is refused and traffic continues"] =
        bad.status === 422 && (await call("/orders/1", ...ACME)).status === 200;
    doc.tenants.globex.perSecond = 1000;
    writeFile(configPath, toYAML(doc));
    results["a valid reload is applied"] = (await fetch(GW + "/admin/reload", { method: "POST" })).status === 200;
    results["the new quota is in force"] = (await call("/orders/1", keys.globex, "192.0.2.10")).status === 200;

    const usage = await (await fetch(GW + "/admin/usage")).json();
    results[`per-tenant usage names globex's hottest path (${usage.globex?.[0]?.path})`] =
        usage.globex?.[0]?.path === "/orders/1" && usage.acme.length === 3;
    const st = await (await fetch(GW + "/admin/status")).json();
    results["route latency percentiles are reported from the digests"] =
        st.latency["/orders/"].p99 >= st.latency["/orders/"].p50 && st.latency["/inventory/"].p50 > 0;
    results["the Trie router picks the longest matching prefix and refuses unknown paths"] =
        live.routeFor("/orders/77")?.prefix === "/orders/" && live.routeFor("/order") === undefined;
    const metrics = await (await fetch(GW + "/metrics")).text();
    results["per-route latency histogram"] = /gateway_seconds_count\{route="\/orders\/"\}/.test(metrics);
    results["structured, traced log lines"] =
        logLines.some((l) => l.event === "config.rejected") && logLines.filter((l) => l.trace).length > 100;

    print("api-platform-gateway");
    print(`  routing     ${JSON.stringify(served)} (weights 5:3:2, one replica flaky)`);
    print(`  breaker     open while orders-c failed, closed after recovery`);
    print(`  cache       ${c1.cache} -> ${c2.cache};  quota burst ${burst.join(",")}`);
    print(`  logs        ${logLines.length} lines; upstream hits ${demo.orderHits}`);
    app.close();
    for (const a of upstreamApps) a.close();
    digester.close();
    removeAll(work);
    check(results);
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
