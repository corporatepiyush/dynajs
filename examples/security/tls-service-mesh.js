// flags: --std
// TLS service mesh — a control plane that enrols workloads, mutual-TLS sidecars that prove identity, per-route
// authorisation, load balancing, retries, circuit breaking, certificate rotation and mesh-wide telemetry.
//
// WHAT IT IS
//   The machinery of Istio/Linkerd in one process, for four services (frontend, orders, inventory x2, payments):
//
//   control plane  an HTTP API that enrols a workload: generates its P-256 key and certificate (SAN
//                  <name>.mesh.local), records its public key, and publishes the trust bundle (all valid mesh
//                  certificates) and the authorisation policy (who may call which route prefix)
//   sidecars       every service gets an inbound sidecar (TLS server, client certificate REQUIRED and checked
//                  against the bundle) and an outbound client. After the TLS handshake the caller answers a
//                  challenge nonce with an ECDSA signature, so the receiver knows WHICH workload it is talking
//                  to, not just that it holds some mesh certificate
//   authorisation  each request is checked against the policy using that proven identity; denials are counted
//   traffic        least-outstanding-requests balancing over replicas, a per-attempt timeout, retries on a
//                  different replica, and a circuit breaker that ejects a replica after consecutive failures
//   rotation       a workload re-enrols with a new key; the bundle carries old and new during the overlap, then
//                  the old certificate is revoked and a connection presenting it is refused
//   telemetry      requests, latencies, denials and retries per source -> destination edge (Prometheus format);
//                  each edge also keeps a TDigest so the report shows true p50/p99 per edge
//   policy index   each sidecar compiles its slice of the policy into a Trie of "caller|route-prefix" keys, so the
//                  per-request RBAC decision is one longestPrefix() lookup, not a scan of every rule
//
// MODULES  dyna:net (TCPServer with TLS, Metrics) · dyna:crypto (ECDSA, X509) · dyna:http (App) · dyna:serialize
//          (MsgPack) · dyna:encoding (uvarint frames) · dyna:hash · dyna:async (withTimeout, sleep) · dyna:file
//          · dyna:structures (Trie, TDigest) · dyna:bytes (concat)
//
// RUN      dynajs --std examples/security/tls-service-mesh.js     enrols the services and runs a day's traffic in seconds

import { TCPServer, Metrics } from "dyna:net";
import * as crypto from "dyna:crypto";
import { App } from "dyna:http";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { PutUvarint, Uvarint } from "dyna:encoding";
import { SHA256Hex } from "dyna:hash";
import { Trie, TDigest } from "dyna:structures";
import { concat } from "dyna:bytes";
import { withTimeout, sleep } from "dyna:async";
import { makeTempDir, writeFile, removeAll } from "dyna:file";
import * as std from "std";

if (typeof crypto.X509?.generateSelfSigned !== "function" || typeof crypto.ECDSA?.generate !== "function") {
    print("tls-service-mesh: this build has no OpenSSL (TLS, X.509, ECDSA); nothing to demonstrate");
    std.exit(0);
}
const work = makeTempDir("mesh");

// =============================================================================
// Control plane
// =============================================================================
const POLICY = [
    { from: "frontend", to: "orders", prefix: "/orders" },
    { from: "frontend", to: "inventory", prefix: "/stock" },
    { from: "orders", to: "inventory", prefix: "/stock" },
    { from: "orders", to: "inventory", prefix: "/reserve" },
    { from: "orders", to: "payments", prefix: "/charge" },
];
const registry = new Map(); // workload id -> { name, publicKey, cert, fingerprint, revoked }
let bundleVersion = 0;
const bundlePath = work.join("bundle.pem");
function writeBundle() {
    const pems = [...registry.values()].filter((w) => !w.revoked).map((w) => w.cert);
    writeFile(bundlePath, pems.join("\n"));
    bundleVersion++;
}
const control = new App({ port: 0 });
control.post("/enrol", (req) => {
    const { name, replica } = JSON.parse(req.body);
    const id = `${name}-${replica}-${SHA256Hex(crypto.RandomBytes(8)).slice(0, 6)}`;
    const { privateKey, publicKey } = crypto.ECDSA.generate("P-256");
    const cert = crypto.X509.generateSelfSigned({
        key: privateKey,
        subject: id,
        days: 1,
        sans: [`${name}.mesh.local`],
    });
    const info = crypto.X509.parse(cert);
    registry.set(id, { name, publicKey, cert, fingerprint: info.fingerprint, revoked: false });
    writeBundle();
    return { id, cert, privateKey, sans: info.sans.dns, notAfter: info.notAfter };
});
control.post("/revoke", (req) => {
    const w = registry.get(JSON.parse(req.body).id);
    if (!w) return { status: 404, body: "" };
    w.revoked = true;
    writeBundle();
    return { revoked: true, bundleVersion };
});
control.get("/identity/:id", (req) => {
    const w = registry.get(req.params.id);
    return w && !w.revoked ? { name: w.name, publicKey: w.publicKey } : { status: 404, body: "" };
});
control.get("/policy", () => POLICY);
control.start();
const cp = `http://127.0.0.1:${control.port}`;

// =============================================================================
// Framing
// =============================================================================
const frame = (obj) => {
    const body = MsgPackEncode(obj),
        len = PutUvarint(body.length),
        out = new Uint8Array(len.length + body.length);
    out.set(len);
    out.set(body, len.length);
    return out;
};
function deframer(onMessage) {
    let buf = new Uint8Array(0);
    return (first, second) => {
        const chunk = second ?? first; // usable as feed(bytes) or as a TCP data(conn, bytes) handler
        const m = new Uint8Array(buf.length + chunk.length);
        m.set(buf);
        m.set(chunk, buf.length);
        buf = m;
        while (buf.length) {
            let n, used;
            try {
                [n, used] = Uvarint(buf);
            } catch {
                return;
            }
            if (buf.length < used + Number(n)) return;
            onMessage(MsgPackDecode(buf.subarray(used, used + Number(n))));
            buf = buf.slice(used + Number(n));
        }
    };
}

// =============================================================================
// Sidecar
// =============================================================================
const stats = { requests: 0, denied: 0, handshakeRefused: 0, retries: 0, ejections: 0 };
class Sidecar {
    constructor(name, replica, handler) {
        this.name = name;
        this.replica = replica;
        this.handler = handler;
    }
    async enrol() {
        const r = await (
            await fetch(cp + "/enrol", {
                method: "POST",
                body: JSON.stringify({ name: this.name, replica: this.replica }),
            })
        ).json();
        this.id = r.id;
        this.privateKey = r.privateKey;
        this.certPath = work.join(`${r.id}.crt`);
        this.keyPath = work.join(`${r.id}.key`);
        writeFile(this.certPath, r.cert);
        writeFile(this.keyPath, r.privateKey);
        this.cert = r.cert;
        return r;
    }
    async listen() {
        this.policy = await (await fetch(cp + "/policy")).json();
        this.rbac = new Trie();
        for (const p of this.policy) if (p.to === this.name) this.rbac.insert(`${p.from}|${p.prefix}`);
        this.server = new TCPServer({
            port: 0,
            tls: { cert: this.certPath, key: this.keyPath, ca: bundlePath, requestCert: true },
        });
        this.server.start({
            connect: (conn) => {
                const nonce = crypto.RandomBytes(16);
                conn.peer = null;
                conn.write(frame({ type: "challenge", nonce, server: this.id }));
                conn.feed = deframer(async (msg) => {
                    if (msg.type === "hello") {
                        const who = await fetch(`${cp}/identity/${encodeURIComponent(msg.id)}`);
                        const ident = who.status === 200 ? await who.json() : (await who.text(), null);
                        if (
                            !ident ||
                            !crypto.ECDSA.verify(
                                "sha256",
                                ident.publicKey,
                                concat([nonce, new TextEncoder().encode(this.id)]),
                                msg.sig,
                            )
                        ) {
                            stats.handshakeRefused++;
                            conn.write(frame({ type: "refused" }));
                            conn.close();
                            return;
                        }
                        conn.peer = ident.name;
                        conn.write(frame({ type: "ready" }));
                        return;
                    }
                    if (msg.type !== "request" || !conn.peer) {
                        conn.close();
                        return;
                    }
                    const ok = this.rbac.longestPrefix(`${conn.peer}|${msg.path}`).length > conn.peer.length + 1;
                    if (!ok) {
                        stats.denied++;
                        Metrics.counter("mesh_denied_total", 1, { source: conn.peer, destination: this.name });
                        conn.write(frame({ type: "response", seq: msg.seq, status: 403, body: "RBAC: access denied" }));
                        return;
                    }
                    const res = await this.handler(msg, conn.peer);
                    try {
                        conn.write(frame({ type: "response", seq: msg.seq, ...res }));
                    } catch {} // the caller may have given up and closed
                });
            },
            data: (conn, bytes) => conn.feed(bytes),
        });
        this.port = this.server.port;
        return this;
    }
    connect(target) {
        return new Promise((resolve, reject) => {
            const pending = new Map();
            let seq = 0,
                ready = false;
            const conn = { pending, request: null, alive: true };
            const client = TCPServer.connect(
                {
                    host: "127.0.0.1",
                    port: target.port,
                    tls: {
                        ca: target.certPath,
                        servername: `${target.name}.mesh.local`,
                        cert: this.certPath,
                        key: this.keyPath,
                    },
                },
                {
                    connect: (c, err) => {
                        if (err) {
                            conn.alive = false;
                            reject(new Error(err));
                        } else conn.raw = c;
                    },
                    data: deframer((msg) => {
                        if (msg.type === "challenge") {
                            const sig = crypto.ECDSA.sign(
                                "sha256",
                                this.privateKey,
                                new Uint8Array([...msg.nonce, ...new TextEncoder().encode(msg.server)]),
                            );
                            conn.raw.write(frame({ type: "hello", id: this.id, sig }));
                        } else if (msg.type === "ready") {
                            ready = true;
                            resolve(conn);
                        } else if (msg.type === "refused") {
                            conn.alive = false;
                            reject(new Error("peer refused our identity"));
                        } else if (msg.type === "response") {
                            pending.get(msg.seq)?.(msg);
                            pending.delete(msg.seq);
                        }
                    }),
                    close: () => {
                        conn.alive = false;
                        for (const f of pending.values()) f({ status: 503, body: "connection closed" });
                        pending.clear();
                        if (!ready) reject(new Error("closed during handshake"));
                    },
                },
            );
            conn.request = (path, body) =>
                new Promise((res) => {
                    const s = ++seq;
                    pending.set(s, res);
                    conn.raw.write(frame({ type: "request", seq: s, path, body }));
                });
            conn.close = () => client.close();
        });
    }
}

// =============================================================================
// Outbound traffic policy: balancing, timeouts, retries, circuit breaking
const edgeLatency = new Map(); // "source -> destination" -> TDigest of ms
// =============================================================================
class Upstream {
    constructor(caller, replicas) {
        this.caller = caller;
        this.replicas = replicas.map((sc) => ({ sc, inflight: 0, fails: 0, ejectedUntil: 0, conn: null }));
    }
    pick(exclude) {
        const now = Date.now();
        const ok = this.replicas.filter((r) => r !== exclude && r.ejectedUntil <= now);
        return ok.sort((a, b) => a.inflight - b.inflight)[0] || null;
    }
    async attempt(r, path, body) {
        r.inflight++; // counted before connecting, so a burst spreads out
        try {
            if (!r.connecting || (r.conn && !r.conn.alive)) {
                r.conn = null;
                r.connecting = withTimeout(this.caller.connect(r.sc), 1500, "connect timeout").then(
                    (c) => (r.conn = c),
                );
                r.connecting.catch(() => {
                    r.connecting = null;
                });
            }
            const conn = await r.connecting;
            return await withTimeout(conn.request(path, body), 300, "attempt timeout");
        } finally {
            r.inflight--;
        }
    }
    async call(path, body) {
        let last = null,
            tried = null;
        for (let attempt = 0; attempt < 3; attempt++) {
            const r = this.pick(tried) || this.pick(null);
            if (!r) break;
            if (attempt) stats.retries++;
            const started = performance.now();
            try {
                const res = await this.attempt(r, path, body);
                Metrics.histogram("mesh_request_ms", performance.now() - started, {
                    source: this.caller.name,
                    destination: r.sc.name,
                });
                edgeLatency
                    .getOrInsertComputed(`${this.caller.name} -> ${r.sc.name}`, () => new TDigest(100))
                    .add(performance.now() - started);
                if (res.status >= 500) throw new Error(`upstream ${res.status}`);
                r.fails = 0;
                stats.requests++;
                Metrics.counter("mesh_requests_total", 1, {
                    source: this.caller.name,
                    destination: r.sc.name,
                    code: String(res.status),
                });
                return res;
            } catch (e) {
                last = e;
                tried = r;
                if (++r.fails >= 3) {
                    r.ejectedUntil = Date.now() + 2000;
                    r.fails = 0;
                    stats.ejections++;
                    r.conn?.close?.();
                    r.conn = null;
                    r.connecting = null;
                }
            }
        }
        return { status: 503, body: `no healthy upstream: ${last?.message}` };
    }
}

// =============================================================================
// The services
// =============================================================================
const stock = new Map([
    ["42", 10],
    ["7", 0],
    ["99", 3],
]);
let inv2Broken = false;
const frontend = new Sidecar("frontend", 1, async () => ({ status: 404, body: "" }));
const orders = new Sidecar("orders", 1, async (msg) => {
    if (msg.path !== "/orders") return { status: 404, body: "" };
    const { sku, qty } = JSON.parse(msg.body);
    const r = await upstreams.ordersToInventory.call("/reserve", JSON.stringify({ sku, qty }));
    if (r.status !== 200) return { status: 409, body: r.body };
    const pay = await upstreams.ordersToPayments.call("/charge", JSON.stringify({ amount: qty * 19.99 }));
    return pay.status === 200
        ? { status: 201, body: JSON.stringify({ order: `o-${Math.floor(Math.random() * 1e6)}`, sku, qty }) }
        : { status: 402, body: pay.body };
});
const inventory = (replica) =>
    new Sidecar("inventory", replica, async (msg) => {
        if (replica === 2 && inv2Broken) {
            await sleep(500);
            return { status: 500, body: "disk full" };
        }
        if (msg.path.startsWith("/stock/"))
            return {
                status: 200,
                body: JSON.stringify({ sku: msg.path.slice(7), qty: stock.get(msg.path.slice(7)) ?? 0, replica }),
            };
        if (msg.path === "/reserve") {
            const { sku, qty } = JSON.parse(msg.body),
                have = stock.get(sku) ?? 0;
            if (have < qty) return { status: 409, body: "insufficient stock" };
            stock.set(sku, have - qty);
            return { status: 200, body: JSON.stringify({ reserved: qty, replica }) };
        }
        return { status: 404, body: "" };
    });
const inv1 = inventory(1),
    inv2 = inventory(2);
const payments = new Sidecar("payments", 1, async (msg) => ({
    status: 200,
    body: JSON.stringify({ charged: JSON.parse(msg.body).amount }),
}));
const all = [frontend, orders, inv1, inv2, payments];
const rogue = new Sidecar("frontend", 9, null); // a compromised host with a genuine mesh certificate
for (const s of [...all, rogue]) await s.enrol();
for (const s of all) await s.listen();
const upstreams = {
    frontToOrders: new Upstream(frontend, [orders]),
    frontToInventory: new Upstream(frontend, [inv1, inv2]),
    frontToPayments: new Upstream(frontend, [payments]),
    ordersToInventory: new Upstream(orders, [inv1, inv2]),
    ordersToPayments: new Upstream(orders, [payments]),
};

// =============================================================================
// Traffic
// =============================================================================
const results = {};
const lookups = await Promise.all(
    Array.from({ length: 40 }, (_, i) => upstreams.frontToInventory.call(`/stock/${["42", "7", "99"][i % 3]}`)),
);
const byReplica = lookups.reduce((m, r) => {
    const rep = JSON.parse(r.body).replica;
    m[rep] = (m[rep] || 0) + 1;
    return m;
}, {});
results[`reads balanced across replicas (${JSON.stringify(byReplica)})`] =
    lookups.every((r) => r.status === 200) && byReplica[1] > 5 && byReplica[2] > 5;
const order = await upstreams.frontToOrders.call("/orders", JSON.stringify({ sku: "42", qty: 2 }));
results["an order flows frontend -> orders -> inventory + payments over mTLS"] =
    order.status === 201 && stock.get("42") === 8;
const oos = await upstreams.frontToOrders.call("/orders", JSON.stringify({ sku: "7", qty: 1 }));
results["business errors pass through unchanged"] = oos.status === 409;
const sneaky = await upstreams.frontToPayments.call("/charge", JSON.stringify({ amount: 1e6 }));
results["frontend may not call payments directly (policy uses the proven identity)"] =
    sneaky.status === 403 && stats.denied >= 1;
const sneaky2 = await upstreams.frontToInventory.call("/reserve", JSON.stringify({ sku: "99", qty: 3 }));
results["route-level policy: frontend may read stock but not reserve it"] =
    sneaky2.status === 403 && stock.get("99") === 3;

// A rogue process with a valid mesh certificate but someone else's identity claim.
rogue.id = orders.id; // claims to be orders, but signs with its own key
let impersonation = "accepted";
try {
    await rogue.connect(payments);
} catch (e) {
    impersonation = e.message;
}
results["claiming another workload's identity fails the signature challenge"] =
    /refused|closed during handshake/.test(impersonation) && stats.handshakeRefused >= 1;
// A client with no mesh certificate at all.
let tlsRefused = false;
await new Promise((resolve) => {
    TCPServer.connect(
        { host: "127.0.0.1", port: payments.port, tls: { ca: payments.certPath, servername: "payments.mesh.local" } },
        {
            connect: (c, err) => {
                if (err) {
                    tlsRefused = true;
                    resolve();
                }
            },
            data: () => {},
            close: () => {
                tlsRefused = true;
                resolve();
            },
        },
    );
    setTimeout(resolve, 1500);
});
results["a client without a mesh certificate cannot complete the handshake"] = tlsRefused;

// A failing replica: retries go elsewhere, then the breaker ejects it.
inv2Broken = true;
const during = await Promise.all(
    Array.from({ length: 20 }, (_, i) => upstreams.frontToInventory.call(`/stock/${i % 2 ? "42" : "99"}`)),
);
results["every request still succeeds while replica 2 is failing"] = during.every(
    (r) => r.status === 200 && JSON.parse(r.body).replica === 1,
);
results[`${stats.retries} retries, ${stats.ejections} ejection(s)`] = stats.retries > 0 && stats.ejections >= 1;
inv2Broken = false;

// Certificate rotation for payments: overlap, cut over, revoke the old one.
const oldCertPath = payments.certPath,
    oldKeyPath = payments.keyPath,
    oldId = payments.id;
payments.server.close();
await payments.enrol();
await payments.listen();
upstreams.ordersToPayments = new Upstream(orders, [payments]);
const afterRotate = await upstreams.frontToOrders.call("/orders", JSON.stringify({ sku: "99", qty: 1 }));
results["traffic flows after payments rotated its key and certificate"] = afterRotate.status === 201;
await (await fetch(cp + "/revoke", { method: "POST", body: JSON.stringify({ id: oldId }) })).text();
const ghost = Object.assign(new Sidecar("payments", 1, null), {
    id: oldId,
    certPath: oldCertPath,
    keyPath: oldKeyPath,
});
let revoked = "accepted";
try {
    await withTimeout(ghost.connect(inv1), 1500, "timeout");
} catch (e) {
    revoked = e.message;
}
results[`the revoked certificate is refused (${revoked})`] = revoked !== "accepted";

const scrape = Metrics.scrape();
results["the Trie RBAC index agrees with a scan of the policy rules"] = [
    "frontend",
    "orders",
    "payments",
    "intruder",
].every((from) =>
    ["inventory", "payments", "orders"].every((to) =>
        ["/stock/1", "/reserve/1", "/charge", "/orders/9", "/", "/stockx"].every((path) => {
            const t = new Trie();
            for (const p of POLICY) if (p.to === to) t.insert(`${p.from}|${p.prefix}`);
            return (
                t.longestPrefix(`${from}|${path}`).length > from.length + 1 ===
                POLICY.some((p) => p.from === from && p.to === to && path.startsWith(p.prefix))
            );
        }),
    ),
);
results["per-edge request metrics are exported"] =
    /mesh_requests_total\{[^}]*source="frontend"[^}]*destination="inventory"/.test(scrape);

print("tls-service-mesh");
print(
    `  enrolled   ${registry.size} workload certificates (P-256, SAN <svc>.mesh.local), bundle v${bundleVersion}, ${[...registry.values()].filter((w) => w.revoked).length} revoked`,
);
print(
    `  latency    ${[...edgeLatency].map(([e, d]) => `${e} p50 ${d.quantile(0.5).toFixed(2)} / p99 ${d.quantile(0.99).toFixed(2)} ms`).join(" | ")}`,
);
print(
    `  traffic    ${stats.requests} requests ok, ${stats.denied} denied by policy, ${stats.handshakeRefused} identity challenges failed`,
);
print(
    `  resilience ${stats.retries} retries, ${stats.ejections} ejections; replica split ${JSON.stringify(byReplica)}`,
);
print(`  rotation   payments ${oldId} -> ${payments.id}; old certificate: ${revoked}`);
for (const s of all) s.server?.close();
control.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
