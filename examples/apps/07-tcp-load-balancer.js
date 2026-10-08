// 07 · TCP load balancer — an L4 proxy spreading connections across HTTP backends.
//
// WHAT IT SHOWS
//   - dyna:net TCPProxy: byte forwarding with no JavaScript on the data path
//   - several upstreams, connection caps and a connect timeout
//   - reading the proxy's counters for a health or metrics endpoint
//
// RUN      dynajs examples/apps/07-tcp-load-balancer.js
// DEPLOY   LISTEN=8080 UPSTREAMS=10.0.0.5:9000,10.0.0.6:9000 dynajs examples/apps/07-tcp-load-balancer.js

import { TCPProxy, HTTPServer } from "dyna:net";
import { getEnv } from "dyna:sys";

const LISTEN = Number(getEnv("LISTEN") ?? 0);
const selfTest = !LISTEN;

// "host:port,host:port" -> [{ host, port }]
const parseUpstreams = (text) => text.split(",").map((pair) => {
    const [host, port] = pair.trim().split(":");
    return { host, port: Number(port) };
});

// In the self-test two tiny backends stand in for real application servers.
// Each answers with its own name so the test can see who served a request.
const backends = [];
if (selfTest) {
    for (const name of ["backend-a", "backend-b"]) {
        const server = new HTTPServer({ port: 0, routes: { "/whoami": name } });
        server.start();
        backends.push(server);
    }
}
const upstream = selfTest
    ? backends.map((b) => ({ host: "127.0.0.1", port: b.port }))
    : parseUpstreams(getEnv("UPSTREAMS") ?? "127.0.0.1:9000");

const proxy = new TCPProxy({
    port: LISTEN || pickPort(),
    upstream,
    maxConns: 4096,              // excess connections are closed and counted as refused
    connectTimeoutMs: 3000,      // a dead upstream fails fast instead of hanging clients
    idleTimeoutMs: 60000,        // a silent pair is reclaimed
});
proxy.start();
console.log(`balancing port ${proxy.port} across ${upstream.length} upstream(s)`);

// The proxy needs a concrete port; in the self-test borrow a free one by
// binding a throwaway server and releasing it.
function pickPort() {
    const probe = new HTTPServer({ port: 0 });
    const port = probe.port;
    probe.close();
    return port;
}

// ---- self-test -------------------------------------------------------------
if (selfTest) {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const seen = new Map();
    for (let i = 0; i < 8; i++) {
        const res = await fetch(`http://127.0.0.1:${proxy.port}/whoami`);
        const who = await res.text();
        seen.set(who, (seen.get(who) ?? 0) + 1);
    }
    check([...seen.keys()].every((k) => k.startsWith("backend-")), "every answer came from a backend");
    check(seen.size === 2, "both upstreams received traffic: " + JSON.stringify([...seen]));

    const stats = proxy.stats();
    check(stats.accepted >= 8 && stats.connectFailed === 0, "the counters saw every connection");
    check(stats.bytesUp > 0 && stats.bytesDown > 0, "bytes flowed both ways");
    console.log("self-test passed:", JSON.stringify([...seen]), "| bytes down:", stats.bytesDown);
    proxy.close();
    for (const b of backends) b.close();
}
