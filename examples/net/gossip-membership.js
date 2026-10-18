// Gossip membership — a SWIM failure detector over real UDP sockets: ping, indirect ping-req, suspicion with
// incarnation numbers and piggybacked dissemination, feeding a consistent-hash ring that moves only the dead node's keys.
//
// WHAT IT IS
//   How Consul/Serf, Cassandra and memberlist know who is alive without a central coordinator:
//
//   nodes       twelve in-process nodes, each with its own UDPSocket on 127.0.0.1; messages are JSON datagrams
//   probe       every protocol period (80 ms) a node pings one member chosen round-robin over a shuffled list; no ack
//               within 30 ms -> ping-req through 3 other members; still nothing -> the target is SUSPECT
//   suspicion   a suspect not refuted within 8 periods is declared DEAD; a live node that hears it is suspected
//               refutes by bumping its incarnation number, and the higher incarnation wins everywhere (even over a
//               DEAD entry, as in memberlist, so a node wrongly declared dead can come back)
//   dissemination every message piggybacks up to 6 recent membership updates, each retransmitted lambda*log(n) times
//   faults      one node is killed (socket closed) and one is made lossy (drops 25% of what it receives); the dead
//               one must be detected by everyone, the lossy one must NOT be declared dead
//   placement   every node keeps a ConsistentHash ring of the members it believes alive; after convergence all
//               rings agree, and only keys owned by the dead node changed owner
//
// MODULES  dyna:net (UDPSocket) · dyna:structures (ConsistentHash, Multiset, TDigest) · dyna:random · dyna:cli ·
//          Array extensions
//
// RUN      dynajs examples/net/gossip-membership.js

import { UDPSocket } from "dyna:net";
import { ConsistentHash, Multiset, TDigest } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(2002);
const PERIOD = 80,
    ACK_TIMEOUT = 30,
    INDIRECT = 3,
    SUSPECT_PERIODS = 8,
    PIGGYBACK = 6;
const N = 12;
const traffic = new Multiset();

class Node {
    constructor(name) {
        this.name = name;
        this.sock = new UDPSocket({ host: "127.0.0.1", port: 0 });
        this.members = new Map();
        this.incarnation = 0;
        this.updates = [];
        this.pending = new Map();
        this.seq = 0;
        this.lossy = 0;
        this.alive = true;
        this.order = [];
        this.cursor = 0;
        this.sock.start({ message: (data) => this.receive(JSON.parse(new TextDecoder().decode(data))) });
    }
    get port() {
        return this.sock.port;
    }
    join(peers) {
        for (const p of peers)
            if (p.name !== this.name) this.members.set(p.name, { port: p.port, state: "alive", inc: 0, since: 0 });
    }
    send(port, msg) {
        if (!this.alive) return;
        msg.from = this.name;
        msg.fromPort = this.port;
        msg.updates = this.updates.slice(0, PIGGYBACK).map((u) => (u.left--, u.u));
        this.updates = this.updates.filter((u) => u.left > 0);
        traffic.add(msg.t);
        this.sock.send(JSON.stringify(msg), "127.0.0.1", port);
    }
    gossip(u) {
        this.updates.unshift({ u, left: Math.ceil(3 * Math.log2(N)) });
    }
    apply(u) {
        if (u.name === this.name) {
            if (u.state !== "alive" && u.inc >= this.incarnation) {
                this.incarnation = u.inc + 1;
                this.gossip({ name: this.name, state: "alive", inc: this.incarnation });
            }
            return;
        }
        const m = this.members.get(u.name);
        if (!m) return;
        const rank = { alive: 0, suspect: 1, dead: 2 };
        if (m.state === "dead" && !(u.state === "alive" && u.inc > m.inc)) return;
        if (u.inc > m.inc || (u.inc === m.inc && rank[u.state] > rank[m.state])) {
            m.inc = u.inc;
            m.state = u.state;
            m.since = performance.now();
            this.gossip(u);
        }
    }
    receive(msg) {
        if (!this.alive || (this.lossy && rng.nextFloat() < this.lossy)) return;
        for (const u of msg.updates ?? []) this.apply(u);
        if (msg.t === "ping") this.send(msg.fromPort, { t: "ack", seq: msg.seq });
        else if (msg.t === "ping-req") {
            const target = this.members.get(msg.target);
            if (!target) return;
            const seq = ++this.seq;
            this.pending.set(seq, () => this.send(msg.fromPort, { t: "ack", seq: msg.seq }));
            this.send(target.port, { t: "ping", seq });
            setTimeout(() => this.pending.delete(seq), ACK_TIMEOUT * 2);
        } else if (msg.t === "ack") {
            const done = this.pending.get(msg.seq);
            if (done) {
                this.pending.delete(msg.seq);
                done();
            }
        }
    }
    probe() {
        if (!this.alive) return;
        const now = performance.now();
        for (const [name, m] of this.members)
            if (m.state === "suspect" && now - m.since > SUSPECT_PERIODS * PERIOD)
                this.apply({ name, state: "dead", inc: m.inc });
        const live = [...this.members].filter(([, m]) => m.state !== "dead").map(([n]) => n);
        if (!live.length) return;
        if (this.cursor >= this.order.length) {
            this.order = live.slice();
            rng.shuffle(this.order);
            this.cursor = 0;
        }
        const name = this.order[this.cursor++],
            m = this.members.get(name);
        if (!m || m.state === "dead") return;
        const seq = ++this.seq;
        let acked = false;
        this.pending.set(seq, () => {
            acked = true;
        });
        this.send(m.port, { t: "ping", seq });
        setTimeout(() => {
            if (acked) return;
            const helpers = live
                .filter((n) => n !== name)
                .sort(() => rng.nextFloat() - 0.5)
                .slice(0, INDIRECT);
            for (const h of helpers) this.send(this.members.get(h).port, { t: "ping-req", seq, target: name });
            setTimeout(() => {
                if (this.pending.has(seq)) {
                    this.pending.delete(seq);
                    if (m.state === "alive") this.apply({ name, state: "suspect", inc: m.inc });
                }
            }, ACK_TIMEOUT * 2);
        }, ACK_TIMEOUT);
    }
    ring() {
        const ch = new ConsistentHash({ replicas: 64 });
        ch.addNode(this.name);
        for (const [n, m] of this.members) if (m.state !== "dead") ch.addNode(n);
        return ch;
    }
    view() {
        return [...this.members]
            .filter(([, m]) => m.state !== "dead")
            .map(([n]) => n)
            .concat(this.name)
            .sort()
            .join(",");
    }
}

// =============================================================================
// Run the cluster
// =============================================================================
const nodes = Array.from({ length: N }, (_, i) => new Node(`n${String(i).padStart(2, "0")}`));
for (const n of nodes) n.join(nodes);
const KEYS = Array.from({ length: 5000 }, (_, i) => `key-${i}`);
const before = nodes[0].ring(),
    ownerBefore = KEYS.map((k) => before.get(k));
const timers = nodes.map((n, i) =>
    setTimeout(() => (n.timer = setInterval(() => n.probe(), PERIOD)), (i * PERIOD) / N),
);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

await sleep(PERIOD * 6);
results["a healthy cluster suspects no one"] = nodes.every((n) =>
    [...n.members.values()].every((m) => m.state === "alive"),
);

const victim = nodes[7],
    flaky = nodes[3];
flaky.lossy = 0.25;
const killedAt = performance.now();
victim.alive = false;
victim.sock.close();
const detection = new TDigest(50);
const detectedAt = new Map();
while (performance.now() - killedAt < 6000) {
    await sleep(PERIOD / 2);
    for (const n of nodes)
        if (n !== victim && !detectedAt.has(n.name) && n.members.get(victim.name).state === "dead") {
            detectedAt.set(n.name, performance.now());
            detection.add(performance.now() - killedAt);
        }
    if (detectedAt.size === N - 1) break;
}
// A false accusation: n00 starts gossiping that the lossy node is suspect. It must hear it and refute.
nodes[0].apply({ name: flaky.name, state: "suspect", inc: flaky.incarnation });
await sleep(PERIOD * 12);
for (const n of nodes) clearInterval(n.timer);
timers.forEach(clearTimeout);
await sleep(PERIOD * 2);

const survivors = nodes.filter((n) => n !== victim);
results["every survivor declares the killed node dead"] = detectedAt.size === N - 1;
results["detection completes within 3 seconds everywhere"] = detection.quantile(1) < 3000;
results["the lossy node is never declared dead"] = survivors.every(
    (n) => n === flaky || n.members.get(flaky.name).state !== "dead",
);
results["a false suspicion is refuted by a higher incarnation everywhere"] =
    flaky.incarnation >= 1 &&
    survivors.every(
        (n) =>
            n === flaky ||
            (n.members.get(flaky.name).state === "alive" && n.members.get(flaky.name).inc === flaky.incarnation),
    );
const views = new Set(survivors.map((n) => n.view()));
results["all survivors converge on the same membership view"] =
    views.size === 1 && !survivors[0].view().includes(victim.name);
{
    const rings = survivors.map((n) => n.ring());
    const agree = KEYS.every((k) => rings.every((r) => r.get(k) === rings[0].get(k)));
    const moved = KEYS.filter((k, i) => rings[0].get(k) !== ownerBefore[i]);
    results["every survivor's ring places every key identically"] = agree;
    results["only the dead node's keys moved"] =
        moved.every((k, i) => ownerBefore[KEYS.indexOf(k)] === victim.name) &&
        moved.length === ownerBefore.filter((o) => o === victim.name).length;
    print(`placement: ${moved.length} of ${KEYS.length} keys moved (all previously on ${victim.name})`);
}
print(
    Table(
        [
            ["detection p50 / max", `${detection.quantile(0.5).toFixed(0)} / ${detection.quantile(1).toFixed(0)} ms`],
            [
                "messages",
                traffic
                    .entrySet()
                    .map(([t, n]) => `${t} ${n}`)
                    .join(", "),
            ],
            ["lossy node incarnation", String(flaky.incarnation)],
        ],
        { head: ["SWIM", "value"] },
    ),
);

for (const n of survivors) n.sock.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
