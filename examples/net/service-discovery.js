// Service discovery — a five-node registry cluster with SWIM failure detection over UDP, a gossiped CRDT service
// catalogue, active HTTP health checks, a DNS front end, and clients that balance with consistent hashing.
//
// WHAT IT IS
//   Consul/Serf in one process, on real sockets:
//
//   membership   each registry node pings a random peer over UDP every round; no ack means it asks K other
//                members to ping indirectly (ping-req); still nothing means SUSPECT, and after a timeout DEAD.
//                A node that hears it is suspected refutes it by bumping its incarnation number. Membership
//                updates piggyback on the pings, so news spreads epidemically (SWIM, Das et al. 2002)
//   catalogue    service registrations are a last-writer-wins map keyed by instance, with (lamport clock, node)
//                versions and tombstones for deregistration — a CRDT, so gossip in any order converges
//   health       the node that owns an instance (by consistent hash) checks it over HTTP; failing checks mark
//                it critical in the catalogue, passing ones bring it back
//   DNS          every node runs a DNS server: `<service>.service.consul` answers with a healthy instance,
//                rotating per query; unknown names get no answer
//   clients      an API client resolves through DNS, and a session-affinity client uses a consistent-hash ring
//                over the healthy set so a user keeps hitting the same instance until it fails
//   chaos        an instance is broken, a registry node is killed, and a network partition isolates one node;
//                detection and convergence times are measured
//   hashing      health-check ownership and session affinity both use the native ConsistentHash ring, so losing a
//                registry node reassigns only the checks it owned (modulo hashing would reshuffle nearly all);
//                gossip de-duplication remembers recent update keys in an LRU instead of re-serialising the
//                whole outbound buffer for every comparison
//
// MODULES  dyna:net (UDPSocket, DNSServer, DNSResolver) · dyna:http (App) · dyna:serialize (MsgPack)
//          · dyna:structures (ConsistentHash, LRU) · dyna:random · dyna:async (sleep) · dyna:cli (Table)
//
// RUN      dynajs examples/net/service-discovery.js     runs the cluster, the services, and the failure drills

import { UDPSocket, DNSServer, DNSResolver } from "dyna:net";
import { App } from "dyna:http";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { ConsistentHash, LRU } from "dyna:structures";
import { Random } from "dyna:random";
import { sleep } from "dyna:async";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const rng = new Random(2002);
const ROUND_MS = 40,
    SUSPECT_MS = 700,
    INDIRECT = 2;
const ringOf = (names) => {
    const r = new ConsistentHash({ replicas: 64 });
    for (const n of names) r.addNode(n);
    return r;
};

// =============================================================================
// Registry node
// =============================================================================
class Node {
    constructor(name) {
        this.name = name;
        this.sock = new UDPSocket({ port: 0, host: "127.0.0.1" });
        this.port = this.sock.port;
        this.members = new Map([[name, { port: this.port, state: "alive", inc: 0, since: Date.now() }]]);
        this.catalog = new Map();
        this.clock = 0;
        this.pending = new Map();
        this.seq = 0;
        this.updates = [];
        this.seen = new LRU(512);
        this.owners = { key: "", ring: null };
        this.partitioned = false;
        this.down = false;
        this.stats = { sent: 0, indirect: 0, refutes: 0 };
        this.sock.start({
            message: (data) => {
                if (!this.down && !this.partitioned) this.onMessage(MsgPackDecode(data));
            },
        });
        this.dnsCounter = 0;
    }
    send(port, msg) {
        if (this.down || this.partitioned) return;
        this.stats.sent++;
        msg.from = this.name;
        msg.gossip = this.updates.slice(-8);
        this.sock.send(MsgPackEncode(msg), "127.0.0.1", port);
    }
    join(seed) {
        this.send(seed.port, { t: "join", port: this.port });
    }
    apply(u) {
        // membership updates: higher incarnation wins, dead is final
        if (u.kind === "member") {
            const cur = this.members.get(u.name);
            if (u.name === this.name && u.state !== "alive") {
                // refute a rumour about ourselves
                if (u.inc >= this.members.get(this.name).inc) {
                    this.members.get(this.name).inc = u.inc + 1;
                    this.stats.refutes++;
                    this.gossip({ kind: "member", name: this.name, port: this.port, state: "alive", inc: u.inc + 1 });
                }
                return;
            }
            const rank = { alive: 0, suspect: 1, dead: 2 };
            if (!cur || u.inc > cur.inc || (u.inc === cur.inc && rank[u.state] > rank[cur.state])) {
                if (cur?.state === "dead" && u.inc <= cur.inc) return;
                this.members.set(u.name, { port: u.port, state: u.state, inc: u.inc, since: Date.now() });
                this.gossip(u);
            }
        } else if (u.kind === "svc") {
            this.clock = Math.max(this.clock, u.ver[0]) + 1;
            const cur = this.catalog.get(u.id);
            if (!cur || u.ver[0] > cur.ver[0] || (u.ver[0] === cur.ver[0] && u.ver[1] > cur.ver[1])) {
                this.catalog.set(u.id, u);
                this.gossip(u);
            }
        }
    }
    gossip(u) {
        const key =
            u.kind === "member" ? `m|${u.name}|${u.state}|${u.inc}` : `s|${u.id}|${u.ver[0]}|${u.ver[1]}|${u.status}`;
        if (this.seen.has(key)) return;
        this.seen.put(key, true);
        this.updates.push(u);
        if (this.updates.length > 64) this.updates.shift();
    }
    onMessage(m) {
        for (const u of m.gossip || []) this.apply(u);
        if (m.t === "join") {
            this.apply({ kind: "member", name: m.from, port: m.port, state: "alive", inc: 0 });
            for (const [n, mm] of this.members)
                this.updates.push({ kind: "member", name: n, port: mm.port, state: mm.state, inc: mm.inc });
            for (const s of this.catalog.values()) this.updates.push(s);
            this.send(m.port, { t: "ack", seq: -1 });
        } else if (m.t === "ping") this.send(this.members.get(m.from)?.port ?? m.replyPort, { t: "ack", seq: m.seq });
        else if (m.t === "ping-req") {
            const target = this.members.get(m.target);
            if (!target) return;
            const seq = ++this.seq;
            this.pending.set(seq, { relayTo: m.from, relaySeq: m.seq, at: Date.now() });
            this.send(target.port, { t: "ping", seq, replyPort: this.port });
        } else if (m.t === "ack") {
            const p = this.pending.get(m.seq);
            if (!p) return;
            this.pending.delete(m.seq);
            if (p.relayTo) this.send(this.members.get(p.relayTo).port, { t: "ack", seq: p.relaySeq });
            else p.acked = true;
        }
    }
    alivePeers() {
        return [...this.members].filter(([n, m]) => n !== this.name && m.state !== "dead");
    }
    async round() {
        if (this.down) return;
        const now = Date.now();
        for (const [n, m] of this.members)
            if (m.state === "suspect" && now - m.since > SUSPECT_MS)
                this.apply({ kind: "member", name: n, port: m.port, state: "dead", inc: m.inc });
        const peers = this.alivePeers();
        if (!peers.length) return;
        const [target, tm] = peers[rng.nextBounded(peers.length)];
        const seq = ++this.seq,
            probe = { at: now };
        this.pending.set(seq, probe);
        this.send(tm.port, { t: "ping", seq, replyPort: this.port });
        await sleep(ROUND_MS / 2);
        if (probe.acked || this.down) return;
        for (const [, helper] of peers.filter(([n]) => n !== target).slice(0, INDIRECT)) {
            this.stats.indirect++;
            this.send(helper.port, { t: "ping-req", seq, target });
        }
        await sleep(ROUND_MS / 2);
        if (!probe.acked && this.members.get(target).state === "alive")
            this.apply({ kind: "member", name: target, port: tm.port, state: "suspect", inc: tm.inc });
    }
    register(svc) {
        this.clock++;
        this.apply({
            kind: "svc",
            id: svc.id,
            service: svc.service,
            port: svc.port,
            status: "passing",
            ver: [this.clock, this.name],
        });
    }
    setStatus(id, status) {
        const cur = this.catalog.get(id);
        if (!cur || cur.status === status) return;
        this.clock++;
        this.apply({ ...cur, status, ver: [this.clock, this.name] });
    }
    healthy(service) {
        return [...this.catalog.values()]
            .filter((s) => s.service === service && s.status === "passing" && !s.deleted)
            .sort((a, b) => a.id.localeCompare(b.id));
    }
    ownerOf(id) {
        const live = [...this.members]
                .filter(([, m]) => m.state === "alive")
                .map(([n]) => n)
                .sort(),
            key = live.join(",");
        if (this.owners.key !== key) this.owners = { key, ring: ringOf(live) };
        return this.owners.ring.get(id);
    }
    startDns() {
        this.dns = new DNSServer({ port: 0, host: "127.0.0.1" });
        this.dns.start((name, type) => {
            const m = /^([a-z0-9-]+)\.service\.consul\.?$/.exec(name.toLowerCase());
            if (!m || type !== 1) return null;
            const h = this.healthy(m[1]);
            return h.length ? `127.0.0.${10 + Number(h[this.dnsCounter++ % h.length].id.split("-").pop())}` : null;
        });
    }
    stop() {
        this.down = true;
        this.sock.close();
        this.dns?.close();
    }
}

// =============================================================================
// The services being discovered: three "api" instances and two "billing"
// =============================================================================
const instances = [];
for (const [service, n] of [
    ["api", 3],
    ["billing", 2],
])
    for (let i = 1; i <= n; i++) {
        const inst = { id: `${service}-${i}`, service, broken: false };
        inst.app = new App({ port: 0 });
        inst.app.get("/health", () => (inst.broken ? { status: 503, body: "db unreachable" } : { status: "ok" }));
        inst.app.get("/hello", () => ({ from: inst.id }));
        inst.app.start();
        inst.port = inst.app.port;
        instances.push(inst);
    }

// =============================================================================
// Cluster
// =============================================================================
const nodes = ["reg-a", "reg-b", "reg-c", "reg-d", "reg-e"].map((n) => new Node(n));
for (const n of nodes.slice(1)) n.join(nodes[0]);
for (const n of nodes) n.startDns();
let running = true;
const loops = nodes.map(async (n) => {
    while (running) {
        await n.round();
        await sleep(rng.nextBounded(10));
    }
});
const healthLoop = (async () => {
    while (running) {
        for (const n of nodes) {
            if (n.down) continue;
            for (const s of [...n.catalog.values()].filter((x) => n.ownerOf(x.id) === n.name)) {
                const inst = instances.find((i) => i.id === s.id);
                let ok = false;
                try {
                    const r = await fetch(`http://127.0.0.1:${inst.port}/health`);
                    await r.text();
                    ok = r.status === 200;
                } catch {}
                n.setStatus(s.id, ok ? "passing" : "critical");
            }
        }
        await sleep(60);
    }
})();
await sleep(300);
nodes[1].register(instances[0]);
nodes[2].register(instances[1]);
nodes[3].register(instances[2]);
nodes[0].register(instances[3]);
nodes[4].register(instances[4]);
const converge = async (pred, limit = 4000) => {
    const t = Date.now();
    while (Date.now() - t < limit) {
        if (pred()) return Date.now() - t;
        await sleep(10);
    }
    return -1;
};
const catalogTime = await converge(() => nodes.every((n) => n.down || n.catalog.size === 5));

const results = {};
results["all five registry nodes see each other alive"] = nodes.every(
    (n) => n.members.size === 5 && [...n.members.values()].every((m) => m.state === "alive"),
);
results[`the catalogue converged on every node in ${catalogTime} ms`] = catalogTime >= 0;

// DNS: rotation over healthy api instances.
const resolver = new DNSResolver({ server: "127.0.0.1", port: nodes[2].dns.port, timeoutMs: 1000 });
const answers = [];
for (let i = 0; i < 6; i++) answers.push((await resolver.lookup("api.service.consul", 1))[0]?.address);
results[`DNS rotates across the three api instances (${[...new Set(answers)].join(", ")})`] =
    new Set(answers).size === 3;
let unknown = "answered";
try {
    const r = await resolver.lookup("nosuch.service.consul", 1);
    unknown = r.length ? "answered" : "empty";
} catch {
    unknown = "no answer";
}
results["an unknown service gets no address"] = unknown !== "answered";

// Consistent-hash affinity over the healthy set.
function ring(node, service) {
    const r = ringOf(node.healthy(service).map((s) => s.id));
    return (key) => r.get(key);
}
const users = Array.from({ length: 300 }, (_, i) => `user-${i}`);
const before = new Map(users.map((u) => [u, ring(nodes[0], "api")(u)]));

// Drill 1: break api-2; its owner's health check marks it critical everywhere.
instances[1].broken = true;
const t1 = await converge(() => nodes.every((n) => n.down || n.catalog.get("api-2").status === "critical"));
const after = new Map(users.map((u) => [u, ring(nodes[0], "api")(u)]));
const moved = users.filter((u) => before.get(u) !== after.get(u));
results[`api-2 marked critical cluster-wide in ${t1} ms; only its ${moved.length} users moved`] =
    t1 >= 0 && moved.every((u) => before.get(u) === "api-2");
const dnsNow = new Set();
for (let i = 0; i < 6; i++) dnsNow.add((await resolver.lookup("api.service.consul", 1))[0]?.address);
results["DNS stops handing out the unhealthy instance"] = !dnsNow.has("127.0.0.12");
instances[1].broken = false;
const t1b = await converge(() => nodes.every((n) => n.down || n.catalog.get("api-2").status === "passing"));
results[`api-2 recovered in ${t1b} ms`] = t1b >= 0;

// Drill 2: kill a registry node. SWIM suspects it, then declares it dead everywhere; its health checks move.
const ownersBefore = new Map(instances.map((i) => [i.id, nodes[0].ownerOf(i.id)]));
const killedAt = Date.now();
nodes[3].stop();
const detect = await converge(
    () => nodes.filter((n) => !n.down).every((n) => n.members.get("reg-d").state === "dead"),
    5000,
);
results[`reg-d declared dead on every survivor ${detect} ms after the kill`] = detect >= 0;
instances[2].broken = true;
const t2 = await converge(() =>
    nodes.filter((n) => !n.down).every((n) => n.catalog.get("api-3").status === "critical"),
);
results["the dead node's health checks were taken over by the new owner"] = t2 >= 0;
const ownersAfter = new Map(instances.map((i) => [i.id, nodes[0].ownerOf(i.id)]));
results["only the checks reg-d owned changed owner"] = instances.every((i) =>
    ownersBefore.get(i.id) === "reg-d"
        ? ownersAfter.get(i.id) !== "reg-d"
        : ownersAfter.get(i.id) === ownersBefore.get(i.id),
);
instances[2].broken = false;

// Drill 3: a short partition of reg-e: suspected, then it refutes with a higher incarnation when it comes back.
nodes[4].partitioned = true;
await converge(() => nodes.slice(0, 3).some((n) => n.members.get("reg-e").state === "suspect"), 3000);
nodes[4].partitioned = false;
const refuted = await converge(
    () => nodes.filter((n) => !n.down).every((n) => n.members.get("reg-e").state === "alive"),
    3000,
);
results[
    `reg-e refuted the suspicion (incarnation ${nodes[4].members.get("reg-e").inc}) instead of being declared dead`
] = refuted >= 0 && nodes[4].stats.refutes >= 1 && nodes[4].members.get("reg-e").inc >= 1;
const indirect = nodes.reduce((s, n) => s + n.stats.indirect, 0);
results[`${indirect} indirect probes were used before suspecting`] = indirect > 0;

running = false;
await Promise.all([...loops, healthLoop]);
print("service-discovery");
print(
    table([
        ["node", "view of members", "catalogue", "packets sent", "indirect", "refutes"],
        ...nodes.map((n) => [
            n.name + (n.down ? " (killed)" : ""),
            [...n.members].map(([k, m]) => `${k.slice(4)}:${m.state[0]}${m.inc}`).join(" "),
            String(n.catalog.size),
            String(n.stats.sent),
            String(n.stats.indirect),
            String(n.stats.refutes),
        ]),
    ]),
);
print(
    `  timings    catalogue ${catalogTime} ms; unhealthy api-2 ${t1} ms; recovery ${t1b} ms; dead registry node ${detect} ms (killed ${Date.now() - killedAt} ms ago)`,
);
print(`  affinity   ${moved.length}/${users.length} sessions moved when api-2 failed (the ideal is ~1/3)`);
resolver.close();
for (const n of nodes) if (!n.down) n.stop();
for (const i of instances) i.app.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
