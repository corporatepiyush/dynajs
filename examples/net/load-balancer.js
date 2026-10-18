// Load balancer — an HTTP L7 balancer in front of four real backends, comparing round robin, least-outstanding and
// power-of-two-choices under a slow backend, with outlier ejection, a retry budget and sticky consistent hashing.
//
// WHAT IT IS
//   The proxy tier of an Envoy/HAProxy deployment, with the policies that matter when one backend degrades:
//
//   backends    four dyna:http Apps; one answers in ~40 ms instead of ~2 ms (a GC-thrashing host), and later one starts
//               returning 503s
//   policies    round robin (blind) · least outstanding requests · power of two choices (pick two at random, take the
//               one with fewer in flight); each serves the same request stream; latency per policy in a TDigest
//   ejection    five consecutive 5xx eject a backend for a cool-down; while ejected it receives no traffic; afterwards it
//               is readmitted and probed
//   retries     a failed request is retried on another backend only while retries stay under 20% of recent requests
//               (a RingBuffer window), so a sick fleet is not hit with a retry storm
//   sticky      session-keyed traffic goes through a ConsistentHash ring; when a backend leaves, only its sessions move
//
// MODULES  dyna:http (App, fetch) · dyna:structures (TDigest, RingBuffer, ConsistentHash, Multiset) · dyna:async (sleep) ·
//          dyna:random · dyna:cli
//
// RUN      dynajs examples/net/load-balancer.js

import { App } from "dyna:http";
import { TDigest, RingBuffer, ConsistentHash, Multiset } from "dyna:structures";
import { sleep } from "dyna:async";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1999);

// =============================================================================
// Backends
// =============================================================================
const backends = [0, 1, 2, 3].map((i) => {
    const b = {
        name: `be-${i}`,
        delay: i === 2 ? 40 : 2,
        failing: false,
        served: 0,
        app: new App({ port: 0 }),
        inflight: 0,
        fails: 0,
        ejectedUntil: 0,
    };
    b.app.get("/work", async () => {
        b.served++;
        await sleep(b.delay + rng.nextBounded(2));
        return b.failing ? { status: 503, body: "unavailable" } : { backend: b.name };
    });
    b.app.start();
    b.url = `http://127.0.0.1:${b.app.port}/work`;
    return b;
});
const now = () => performance.now();
const healthy = () => backends.filter((b) => b.ejectedUntil <= now());

// =============================================================================
// Balancer
// =============================================================================
const policies = {
    "round robin": (() => {
        let i = 0;
        return () => {
            const h = healthy();
            return h[i++ % h.length];
        };
    })(),
    "least outstanding": () => healthy().reduce((a, b) => (b.inflight < a.inflight ? b : a)),
    "power of two": () => {
        const h = healthy();
        const a = h[rng.nextBounded(h.length)],
            b = h[rng.nextBounded(h.length)];
        return a.inflight <= b.inflight ? a : b;
    },
};
const window = new RingBuffer(200);
const counters = new Multiset();
async function forward(pick, exclude = null) {
    let b = pick();
    if (b === exclude && healthy().length > 1) b = healthy().find((x) => x !== exclude);
    if (b.ejectedUntil > now()) counters.add("routed-to-ejected");
    b.inflight++;
    const t = now();
    let status;
    try {
        status = (await fetch(b.url)).status;
    } finally {
        b.inflight--;
    }
    if (status >= 500) {
        if (++b.fails >= 5) {
            b.ejectedUntil = now() + 400;
            b.fails = 0;
            counters.add(`ejected:${b.name}`);
        }
    } else b.fails = 0;
    return { status, ms: now() - t, backend: b };
}
async function request(pick) {
    const first = await forward(pick);
    window.push(0);
    if (first.status < 500) return first;
    const recent = window.toArray(),
        retries = recent.filter((x) => x === 1).length;
    if (retries >= 0.2 * recent.length) {
        counters.add("retry-denied");
        return first;
    }
    window.push(1);
    counters.add("retried");
    return forward(pick, first.backend);
}

// =============================================================================
// Policy comparison with one slow backend
// =============================================================================
const table = [],
    lat = {};
for (const [name, pick] of Object.entries(policies)) {
    for (const b of backends) b.served = 0;
    const d = new TDigest(100);
    let sum = 0,
        n = 0;
    // Closed loop: 12 clients, each sending its next request when the previous one returns, so in-flight counts
    // reflect how fast each backend drains (a single burst would see equal counts everywhere).
    await Promise.all(
        Array.from({ length: 12 }, async () => {
            for (let k = 0; k < 40; k++) {
                const ms = (await request(pick)).ms;
                d.add(ms);
                sum += ms;
                n++;
            }
        }),
    );
    lat[name] = {
        mean: sum / n,
        p90: d.quantile(0.9),
        p50: d.quantile(0.5),
        p99: d.quantile(0.99),
        slowShare: backends[2].served / backends.reduce((s, b) => s + b.served, 0),
    };
    table.push([
        name,
        lat[name].mean.toFixed(1),
        lat[name].p50.toFixed(1),
        lat[name].p90.toFixed(1),
        lat[name].p99.toFixed(1),
        (100 * lat[name].slowShare).toFixed(1) + "%",
    ]);
}
print(Table(table, { head: ["policy", "mean ms", "p50 ms", "p90 ms", "p99 ms", "share sent to the slow backend"] }));
results["round robin sends the slow backend a full quarter"] = Math.abs(lat["round robin"].slowShare - 0.25) < 0.02;
results["least-outstanding starves the slow backend"] = lat["least outstanding"].slowShare < 0.15;
results["power of two choices avoids it too"] = lat["power of two"].slowShare < 0.2;
results["load-aware policies cut mean latency versus round robin"] =
    lat["least outstanding"].mean < lat["round robin"].mean && lat["power of two"].mean < lat["round robin"].mean;

// =============================================================================
// Outlier ejection and the retry budget
// =============================================================================
{
    backends[1].failing = true;
    const pick = policies["round robin"],
        statuses = new Multiset();
    // Checked where routing happens: a request may never be sent to a backend whose ejection is in force.
    for (let wave = 0; wave < 10; wave++) {
        const rs = await Promise.all(Array.from({ length: 30 }, () => request(pick)));
        for (const r of rs) statuses.add(String(r.status));
    }
    results["a failing backend is ejected"] = counters.count("ejected:be-1") > 0;
    results["an ejected backend receives no traffic while ejected"] = counters.count("routed-to-ejected") === 0;
    results["retries rescue most failures"] =
        statuses.count("200") / (statuses.count("200") + statuses.count("503")) > 0.95;
    backends[1].failing = false;
    await sleep(450);
    const back = await Promise.all(Array.from({ length: 40 }, () => request(pick)));
    results["after the cool-down the backend is readmitted"] = back.some(
        (r) => r.backend === backends[1] && r.status === 200,
    );
}
{
    for (const b of backends) b.failing = true;
    const rs = await Promise.all(Array.from({ length: 60 }, () => request(policies["round robin"])));
    for (const b of backends) {
        b.failing = false;
        b.ejectedUntil = 0;
    }
    results["when every backend fails, the retry budget caps retries"] =
        counters.count("retry-denied") > 0 && rs.every((r) => r.status === 503 || r.status === 200);
}

// =============================================================================
// Sticky sessions on a consistent-hash ring
// =============================================================================
{
    const ring = new ConsistentHash({ replicas: 100 });
    for (const b of backends) ring.addNode(b.name);
    const sessions = Array.from({ length: 2000 }, (_, i) => `session-${i}`);
    const before = sessions.map((s) => ring.get(s));
    const load = new Multiset();
    before.forEach((b) => load.add(b));
    ring.removeNode("be-3");
    const after = sessions.map((s) => ring.get(s));
    const moved = sessions.filter((_, i) => before[i] !== after[i]);
    results["sticky sessions spread within 25% of even"] = load.entrySet().every(([, n]) => Math.abs(n - 500) < 125);
    results["removing a backend moves only its own sessions"] =
        moved.every((s) => before[sessions.indexOf(s)] === "be-3") && moved.length === load.count("be-3");
    const real = await Promise.all(
        sessions.slice(0, 40).map((s) => forward(() => backends.find((b) => b.name === ring.get(s)))),
    );
    results["sticky requests land on the ring's backend"] = real.every(
        (r, i) => r.backend.name === ring.get(sessions[i]),
    );
}

print(
    `retries: ${counters.count("retried")}, denied by budget: ${counters.count("retry-denied")}, ejections: ${counters
        .entrySet()
        .filter(([k]) => k.startsWith("ejected"))
        .map(([k, n]) => `${k.slice(8)}x${n}`)
        .join(" ")}`,
);
for (const b of backends) b.app.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
