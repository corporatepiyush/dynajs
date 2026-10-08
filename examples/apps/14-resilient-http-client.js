// 14 · Resilient HTTP client — timeouts, retries with backoff, a circuit breaker and bounded concurrency.
//
// WHAT IT SHOWS
//   - fetch with a per-request timeout
//   - dyna:async retry: exponential backoff, retrying only what is worth retrying
//   - a small circuit breaker so a dead dependency fails fast instead of piling up
//   - dyna:async pmap: many calls with a ceiling on how many run at once
//
// RUN      dynajs examples/apps/14-resilient-http-client.js
//          The file starts a deliberately flaky server and calls it; copy the
//          client half (callApi, CircuitBreaker) into your own service.

import { App } from "dyna:net";
import { retry, pmap } from "dyna:async";

// ---- the client ------------------------------------------------------------
class HttpError extends Error {
    constructor(status) { super("HTTP " + status); this.status = status; }
}

// Opens after `threshold` consecutive failures and refuses calls for
// `cooldownMs`; then lets one probe through (half-open) to test recovery.
class CircuitBreaker {
    constructor({ threshold = 5, cooldownMs = 1000 } = {}) {
        this.threshold = threshold; this.cooldownMs = cooldownMs;
        this.failures = 0; this.openedAt = 0; this.rejected = 0;
    }
    get state() {
        if (this.failures < this.threshold) return "closed";
        return Date.now() - this.openedAt >= this.cooldownMs ? "half-open" : "open";
    }
    async run(fn) {
        if (this.state === "open") { this.rejected++; throw new Error("circuit open"); }
        try {
            const value = await fn();
            this.failures = 0;                       // any success closes the circuit
            return value;
        } catch (e) {
            if (++this.failures >= this.threshold) this.openedAt = Date.now();
            throw e;
        }
    }
}

// 4xx is the caller's fault and will not improve on a retry; 5xx, timeouts
// and connection errors might.
const retryable = (e) => !(e instanceof HttpError) || e.status >= 500;

async function callApi(url, { breaker, retries = 3, timeoutMs = 500, onRetry } = {}) {
    const attempt = async () => {
        const res = await fetch(url, { timeout: timeoutMs });
        if (!res.ok) throw new HttpError(res.status);
        return res.json();
    };
    const guarded = () => (breaker ? breaker.run(attempt) : attempt());
    return retry(async () => {
        try { return await guarded(); }
        catch (e) {
            if (!retryable(e)) { e.final = true; return Promise.reject(e); }
            throw e;
        }
    }, {
        retries, backoffMs: 10, factor: 2,
        // Throwing from onRetry ends the retry loop with that error.
        onRetry: (err, n) => { if (err.final) throw err; onRetry?.(err, n); },
    });
}

// ---- a flaky server to practice on -----------------------------------------
let flakyCalls = 0;
const app = new App({ port: 0 });
// Fails twice, then succeeds: the classic transient error.
app.get("/flaky", () => (++flakyCalls % 3 === 0 ? { ok: true, attempt: flakyCalls } : { status: 503, body: "busy" }));
app.get("/missing", () => ({ status: 404, body: "no" }));
app.get("/slow", async () => { await sleep(300); return { ok: true }; });
app.get("/down", () => ({ status: 500, body: "boom" }));
app.get("/item/:id", async (req) => { await sleep(5); return { id: Number(req.params.id) }; });
app.start();
const base = `http://127.0.0.1:${app.port}`;

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const outcome = (p) => p.then((v) => ({ ok: v }), (e) => ({ err: e.message }));

let retriesSeen = 0;
const flaky = await callApi(base + "/flaky", { onRetry: () => retriesSeen++ });
check(flaky.ok && retriesSeen === 2, "a transient 503 succeeds on the third attempt");

const before = flakyCalls;
check((await outcome(callApi(base + "/missing"))).err === "HTTP 404", "a 404 surfaces as an error");
check(flakyCalls === before, "and is not retried");

check("err" in await outcome(callApi(base + "/slow", { timeoutMs: 50, retries: 0 })), "a slow call times out");

// The breaker: after 3 failures it stops calling the dependency at all.
const breaker = new CircuitBreaker({ threshold: 3, cooldownMs: 80 });
for (let i = 0; i < 6; i++) await outcome(callApi(base + "/down", { breaker, retries: 0 }));
check(breaker.state === "open" && breaker.rejected === 3, "the circuit opens and fails fast");
await sleep(100);
check(breaker.state === "half-open", "after the cooldown one probe is allowed");
check((await callApi(base + "/item/1", { breaker })).id === 1 && breaker.state === "closed", "a success closes it");

// Fan out 20 calls, at most 4 in flight, results in input order.
const ids = Array.from({ length: 20 }, (_, i) => i + 1);
const items = await pmap(ids, (id) => callApi(`${base}/item/${id}`), { concurrency: 4 });
check(items.every((item, i) => item.id === ids[i]), "pmap keeps input order");
console.log("self-test passed: retry, no-retry on 4xx, timeout, circuit breaker, bounded fan-out");
app.close();
