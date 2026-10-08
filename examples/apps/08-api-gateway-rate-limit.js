// 08 · API gateway — API-key authentication, per-key rate limits and request metrics in middleware.
//
// WHAT IT SHOWS
//   - App.use: middleware that can short-circuit with its own response
//   - dyna:net RateLimiter: a token bucket per key, in a fixed-size table (no unbounded growth)
//   - dyna:net Metrics: counters and histograms in Prometheus text format
//
// RUN      dynajs examples/apps/08-api-gateway-rate-limit.js
// DEPLOY   PORT=8080 API_KEYS=key1:acme,key2:globex dynajs examples/apps/08-api-gateway-rate-limit.js

import { App, RateLimiter, Metrics } from "dyna:net";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);

// key -> tenant. In production load these from a secret store, not the environment.
const tenants = new Map((getEnv("API_KEYS") ?? "k-acme:acme,k-globex:globex")
    .split(",").map((pair) => pair.split(":")));

// 5 requests per second sustained, bursts of 10. The table has a fixed number
// of slots, so a flood of made-up keys cannot grow memory.
const limiter = new RateLimiter({ tokensPerSec: 5, burst: 10 });

const json = (status, value) => ({ status, contentType: "application/json", body: JSON.stringify(value) });

const app = new App({ port: PORT });

// Middleware runs in registration order with the same request object the
// handler sees, so it can annotate the request for later stages.
app.use((req) => {
    req.startedAt = Date.now();
    if (req.path === "/metrics") return;                    // scrapes are not tenant traffic
    const tenant = tenants.get(req.headers["x-api-key"] ?? "");
    if (!tenant) {
        Metrics.counter("gateway_rejected_total", 1, { reason: "auth" });
        return { response: json(401, { error: "missing or unknown API key" }) };
    }
    if (!limiter.allow(tenant)) {
        Metrics.counter("gateway_rejected_total", 1, { reason: "rate" });
        return { response: json(429, { error: "rate limit exceeded", retryAfterSec: 1 }) };
    }
    req.tenant = tenant;
});

// Handlers stay free of cross-cutting concerns: by the time they run the
// caller is authenticated and within budget.
function observed(name, handler) {
    return (req) => {
        const result = handler(req);
        Metrics.counter("gateway_requests_total", 1, { route: name, tenant: req.tenant });
        Metrics.histogram("gateway_request_seconds", (Date.now() - req.startedAt) / 1000, { route: name });
        return result;
    };
}

app.get("/v1/quote/:symbol", observed("quote", (req) =>
    json(200, { symbol: req.params.symbol.toUpperCase(), price: 101.25, tenant: req.tenant })));
app.get("/v1/whoami", observed("whoami", (req) => json(200, { tenant: req.tenant })));
app.get("/metrics", () => ({ contentType: "text/plain", body: Metrics.scrape() }));

app.start();
console.log("gateway on port", app.port);

// ---- self-test -------------------------------------------------------------
if (!PORT) {
    const base = `http://127.0.0.1:${app.port}`;
    const get = (path, key) => fetch(base + path, { headers: key ? { "x-api-key": key } : {} });
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };

    check((await get("/v1/whoami")).status === 401, "no key is 401");
    check((await get("/v1/whoami", "wrong")).status === 401, "an unknown key is 401");
    const me = await (await get("/v1/whoami", "k-acme")).json();
    check(me.tenant === "acme", "a valid key resolves its tenant");

    // Burn through the burst: some of 30 rapid calls must be refused.
    let ok = 0, limited = 0;
    for (let i = 0; i < 30; i++) {
        const status = (await get("/v1/quote/abc", "k-acme")).status;
        if (status === 200) ok++; else if (status === 429) limited++;
    }
    check(ok >= 9 && limited > 0, `the bucket admits the burst then refuses (ok=${ok}, limited=${limited})`);

    // Another tenant has its own bucket and is unaffected.
    check((await get("/v1/quote/xyz", "k-globex")).status === 200, "tenants do not share a budget");

    const scrape = await (await get("/metrics")).text();
    check(scrape.includes('gateway_rejected_total{reason="rate"}'), "rejections are counted by reason");
    check(scrape.includes("gateway_request_seconds_bucket"), "latency histogram is exported");
    console.log(`self-test passed: ${ok} admitted, ${limited} rate-limited`);
    app.close();
}
