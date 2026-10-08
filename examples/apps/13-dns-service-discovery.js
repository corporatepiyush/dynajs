// 13 · DNS service discovery — an internal DNS server for *.svc.internal, queried with a caching resolver.
//
// WHAT IT SHOWS
//   - dyna:net DNSServer: answer A queries from a registry held in memory
//   - dyna:net DNSResolver: promise lookups, a TTL answer cache, and failure handling
//   - a registry with health state, so discovery only returns live instances
//
// RUN      dynajs examples/apps/13-dns-service-discovery.js
// DEPLOY   PORT=5353 dynajs examples/apps/13-dns-service-discovery.js
//          then: dig @127.0.0.1 -p 5353 api.svc.internal

import { DNSServer, DNSResolver } from "dyna:net";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const ZONE = ".svc.internal";
const TYPE_A = 1;

// service name -> instances. Round-robin position is kept per service so
// successive queries spread across the healthy instances.
const registry = new Map();
function register(service, address) {
    const entry = registry.get(service) ?? { instances: [], next: 0 };
    entry.instances.push({ address, healthy: true });
    registry.set(service, entry);
}
function setHealth(service, address, healthy) {
    const inst = registry.get(service)?.instances.find((i) => i.address === address);
    if (inst) inst.healthy = healthy;
}

// The handler maps (name, type) to an address, or null for "no such name".
// Names arrive as the client sent them, so compare case-insensitively.
function resolve(name, type) {
    const lower = name.toLowerCase().replace(/\.$/, "");
    if (type !== TYPE_A || !lower.endsWith(ZONE)) return null;      // not our zone
    const entry = registry.get(lower.slice(0, -ZONE.length));
    if (!entry) return null;
    const live = entry.instances.filter((i) => i.healthy);
    if (!live.length) return null;                                  // all down: answer nothing
    return live[entry.next++ % live.length].address;
}

register("api", "10.0.1.10");
register("api", "10.0.1.11");
register("db", "10.0.2.5");

const server = new DNSServer({ port: PORT, host: "127.0.0.1" });
server.start(resolve);
console.log("DNS for *" + ZONE + " on UDP port", server.port);

// ---- self-test -------------------------------------------------------------
if (!PORT) {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    // No cache here: the test wants to see every answer the server gives.
    const resolver = new DNSResolver({ server: "127.0.0.1", port: server.port, timeoutMs: 1000 });
    const lookup = async (name) => {
        try { return (await resolver.lookup(name)).map((r) => r.address); }
        catch (e) { return []; }                                    // NXDOMAIN or timeout
    };

    check((await lookup("db.svc.internal")).join() === "10.0.2.5", "a single instance resolves");

    const seen = new Set();
    for (let i = 0; i < 4; i++) for (const a of await lookup("API.svc.internal")) seen.add(a);
    check(seen.size === 2, "round robin returns both api instances: " + [...seen]);

    setHealth("api", "10.0.1.10", false);
    const afterFailure = new Set();
    for (let i = 0; i < 4; i++) for (const a of await lookup("api.svc.internal")) afterFailure.add(a);
    check([...afterFailure].join() === "10.0.1.11", "an unhealthy instance drops out of discovery");

    check((await lookup("nothing.svc.internal")).length === 0, "an unknown service has no answer");
    check((await lookup("example.com")).length === 0, "names outside the zone are not answered");

    // A caching resolver: the second lookup is served without a wire query,
    // so it keeps returning the first answer for the TTL.
    const cached = new DNSResolver({ server: "127.0.0.1", port: server.port, timeoutMs: 1000, ttl: 30 });
    const first = (await cached.lookup("db.svc.internal"))[0].address;
    setHealth("db", "10.0.2.5", false);
    const second = (await cached.lookup("db.svc.internal"))[0].address;
    check(first === second, "the cache serves the answer while the TTL lasts");
    console.log("self-test passed: discovery, round robin, health, caching");
    cached.close();
    resolver.close();
    server.close();
}
