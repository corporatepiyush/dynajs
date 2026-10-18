// Multiplayer game server — matchmaking by rating, an authoritative fixed-tick simulation over WebSockets, client
// prediction with server reconciliation, delta-compressed binary snapshots, interest management, lag-compensated
// hit detection, speed-hack detection and Elo updates.
//
// WHAT IT IS
//   The netcode of a small top-down arena shooter, with eight bot clients that play over real sockets:
//
//   lobby        clients queue with a rating; the matchmaker forms 4-player matches from the closest ratings,
//                widening the allowed gap the longer someone waits
//   simulation   the server runs at 20 ticks per second and is the only authority; clients send inputs
//                (move vector, aim, fire) stamped with a sequence number and the tick they saw
//   prediction   each client applies its own inputs immediately, keeps the unacknowledged ones, and when a
//                snapshot arrives rewinds to the server's state and replays them (reconciliation); the
//                prediction error is measured
//   snapshots    binary: varint-encoded, positions quantised to 1/16 unit, and only fields that changed since the
//                last snapshot the client ACKNOWLEDGED (delta compression), sized against full snapshots
//   interest     a spatial grid: a client only receives entities within its view radius
//   lag comp     the server keeps 1 s of position history; a shot is resolved against where the targets were at
//                the shooter's view tick, so a 120 ms client still hits what it aimed at
//   anti-cheat   moves faster than the speed cap are clamped and counted; repeat offenders are kicked
//   results      kills update an Elo rating per player at match end; a ZSet holds the global ladder
//   pickups      300 health packs are static, so they live in one KDTree per match: each snapshot's visible packs
//                are a radius() query and a pickup is a radius(1) query — no per-tick scan of every pack; a
//                collected pack respawns 40 ticks later (a SortedMap keyed by respawn tick)
//
// MODULES  dyna:http (App ws, WsClient) · dyna:encoding (varints) · dyna:bytes (concat) · dyna:structures (RingBuffer,
//          KDTree, SortedMap, ZSet) · dyna:random · dyna:async (sleep) · dyna:cli (Table) · dyna:mathx (stats)
//
// RUN      dynajs examples/net/multiplayer-game.js     8 bots queue, two matches play for ~6 simulated seconds each

import { App, WsClient } from "dyna:http";
import { PutVarint, Varint, PutUvarint, Uvarint } from "dyna:encoding";
import { RingBuffer, KDTree, SortedMap, ZSet } from "dyna:structures";
import { concat } from "dyna:bytes";
import { Random } from "dyna:random";
import { sleep } from "dyna:async";
import { Table } from "dyna:cli";
import { stats } from "dyna:mathx";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const TICK_MS = 50,
    SPEED = 6,
    VIEW = 30,
    ARENA = 60,
    HISTORY = 20;
const rng = new Random(20);

// =============================================================================
// Shared movement rule (client and server run the same code — prediction depends on it)
// =============================================================================
function step(p, input) {
    const len = Math.hypot(input.dx, input.dy) || 1,
        k = Math.min(1, len);
    p.x = Math.max(0, Math.min(ARENA, p.x + ((input.dx / len) * k * SPEED * TICK_MS) / 1000));
    p.y = Math.max(0, Math.min(ARENA, p.y + ((input.dy / len) * k * SPEED * TICK_MS) / 1000));
}
const q = (v) => Math.round(v * 16);

// =============================================================================
// Snapshot codec: [tick][baseTick][count] then per entity [id][mask][fields…]
// =============================================================================
const FIELDS = ["x", "y", "hp", "score"];
function encodeSnapshot(tick, baseTick, ents, base) {
    const parts = [PutUvarint(tick), PutUvarint(baseTick), PutUvarint(ents.length)];
    for (const e of ents) {
        const prev = base?.get(e.id),
            vals = [q(e.x), q(e.y), e.hp, e.score];
        let mask = 0;
        vals.forEach((v, i) => {
            if (!prev || prev[i] !== v) mask |= 1 << i;
        });
        parts.push(PutUvarint(e.id), PutUvarint(mask));
        vals.forEach((v, i) => {
            if (mask & (1 << i)) parts.push(PutVarint(prev ? v - prev[i] : v));
        });
    }
    return concat(parts);
}
function decodeSnapshot(buf, baseState) {
    let off = 0;
    const u = () => {
        const [v, n] = Uvarint(buf.subarray(off));
        off += n;
        return Number(v);
    };
    const s = () => {
        const [v, n] = Varint(buf.subarray(off));
        off += n;
        return Number(v);
    };
    const tick = u(),
        baseTick = u(),
        count = u(),
        state = new Map();
    for (let i = 0; i < count; i++) {
        const id = u(),
            mask = u(),
            prev = baseState?.get(id),
            vals = prev ? [...prev] : [0, 0, 0, 0];
        for (let f = 0; f < FIELDS.length; f++) if (mask & (1 << f)) vals[f] = prev ? prev[f] + s() : s();
        state.set(id, vals);
    }
    return { tick, baseTick, state };
}

// =============================================================================
// Server: lobby, matchmaking, matches
// =============================================================================
const server = {
    queue: [],
    matches: [],
    clients: new Map(),
    ratings: new Map(),
    ladder: new ZSet(),
    stats: {
        snapshots: 0,
        deltaBytes: 0,
        fullBytes: 0,
        clamped: 0,
        kicked: 0,
        hits: 0,
        lagCompHits: 0,
        pickups: 0,
        interestMismatches: 0,
    },
};
const PACKS = 300,
    PACK_ID = 1000,
    RESPAWN_TICKS = 40;
class Match {
    constructor(id, players) {
        this.id = id;
        this.tick = 0;
        this.players = new Map();
        this.history = new RingBuffer(HISTORY);
        this.events = [];
        this.over = false;
        const r = new Random(id.length * 7919 + players.length);
        this.packs = Array.from({ length: PACKS }, () => [
            r.nextBounded(ARENA * 16) / 16,
            r.nextBounded(ARENA * 16) / 16,
        ]);
        this.packTree = new KDTree(this.packs);
        this.packGone = new Set();
        this.respawns = new SortedMap(); // tick -> pack ids
        players.forEach((c, i) => {
            this.players.set(c.id, {
                id: c.id,
                client: c,
                x: 15 + (i % 2) * 22,
                y: 15 + Math.floor(i / 2) * 22,
                hp: 100,
                score: 0,
                kills: 0,
                lastSeq: 0,
                inputs: [],
                acked: null,
                sent: new Map(),
                strikes: 0,
            });
            c.match = this;
        });
        for (const p of this.players.values())
            p.client.ws.send(JSON.stringify({ type: "start", match: id, you: p.id, x: p.x, y: p.y, tickMs: TICK_MS }));
    }
    run() {
        this.timer = setInterval(() => {
            this.tick++;
            for (const p of this.players.values()) {
                for (const inp of p.inputs.splice(0)) {
                    const before = { x: p.x, y: p.y };
                    if (Math.hypot(inp.dx, inp.dy) > 1.05) {
                        server.stats.clamped++;
                        if (++p.strikes > 15) this.kick(p, "speed hack");
                    }
                    step(p, inp);
                    p.lastSeq = inp.seq;
                    for (const k of this.packTree.radius([p.x, p.y], 1).indices) {
                        if (this.packGone.has(k)) continue;
                        this.packGone.add(k);
                        server.stats.pickups++;
                        p.hp = Math.min(100, p.hp + 25);
                        const at = this.tick + RESPAWN_TICKS;
                        (this.respawns.get(at) || this.respawns.set(at, []).get(at)).push(k);
                    }
                    if (inp.fire) this.shoot(p, inp, before);
                }
            }
            for (const [t, ids] of this.respawns.rangeQuery(0, this.tick)) {
                for (const k of ids) this.packGone.delete(k);
                this.respawns.delete(t);
            }
            this.history.push({
                tick: this.tick,
                pos: new Map([...this.players.values()].map((p) => [p.id, [p.x, p.y]])),
            });
            for (const p of this.players.values()) this.snapshot(p);
            if (this.tick >= 120) this.finish();
        }, TICK_MS);
    }
    shoot(p, inp, from) {
        // hitscan resolved against the shooter's view of the world
        const view = this.history.toArray().find((h) => h.tick === inp.viewTick) || this.history.toArray().at(-1);
        if (!view) return;
        let best = null;
        for (const [id, [tx, ty]] of view.pos) {
            if (id === p.id || !this.players.has(id) || this.players.get(id).hp <= 0) continue;
            const vx = tx - from.x,
                vy = ty - from.y,
                t = vx * inp.ax + vy * inp.ay;
            if (t < 0 || t > 40) continue;
            const miss = Math.hypot(vx - t * inp.ax, vy - t * inp.ay);
            if (miss < 1.2 && (!best || t < best.t)) best = { id, t };
        }
        if (!best) return;
        const target = this.players.get(best.id),
            now = target && Math.hypot(target.x - view.pos.get(best.id)[0], target.y - view.pos.get(best.id)[1]);
        server.stats.hits++;
        if (now > 0.3) server.stats.lagCompHits++;
        target.hp -= 34;
        if (target.hp <= 0) {
            p.kills++;
            p.score += 100;
            this.events.push({ tick: this.tick, killer: p.id, victim: target.id });
            target.hp = 100;
            target.x = rng.nextBounded(ARENA);
            target.y = rng.nextBounded(ARENA);
        }
    }
    snapshot(p) {
        const visible = [...this.players.values()].filter((o) => Math.hypot(o.x - p.x, o.y - p.y) <= VIEW || o === p);
        const near = [...this.packTree.radius([p.x, p.y], VIEW).indices].filter((k) => !this.packGone.has(k));
        if (this.tick % 30 === 0) {
            // spot-check the index against a scan
            const scan = this.packs
                .map((xy, k) => k)
                .filter(
                    (k) => !this.packGone.has(k) && Math.hypot(this.packs[k][0] - p.x, this.packs[k][1] - p.y) <= VIEW,
                );
            if (scan.length !== near.length) server.stats.interestMismatches++;
        }
        for (const k of near)
            visible.push({ id: PACK_ID + k, x: this.packs[k][0], y: this.packs[k][1], hp: 1, score: 0 });
        const base = p.acked !== null ? p.sent.get(p.acked) : null;
        const full = encodeSnapshot(this.tick, 0, visible, null);
        const delta = encodeSnapshot(this.tick, base ? p.acked : 0, visible, base);
        server.stats.snapshots++;
        server.stats.fullBytes += full.length;
        server.stats.deltaBytes += delta.length;
        p.sent.set(this.tick, new Map(visible.map((e) => [e.id, [q(e.x), q(e.y), e.hp, e.score]])));
        for (const t of p.sent.keys()) if (t < this.tick - 40) p.sent.delete(t);
        p.client.ws.send(JSON.stringify({ type: "snap", lastSeq: p.lastSeq, data: Array.from(delta) }));
    }
    kick(p, why) {
        server.stats.kicked++;
        p.client.ws.send(JSON.stringify({ type: "kicked", why }));
        this.players.delete(p.id);
    }
    finish() {
        clearInterval(this.timer);
        this.over = true;
        const ps = [...this.players.values()];
        for (const a of ps)
            for (const b of ps)
                if (a.id < b.id) {
                    // pairwise Elo by kills
                    const ra = server.ratings.get(a.id),
                        rb = server.ratings.get(b.id),
                        ea = 1 / (1 + 10 ** ((rb - ra) / 400));
                    const sa = a.kills === b.kills ? 0.5 : a.kills > b.kills ? 1 : 0;
                    server.ratings.set(a.id, ra + 16 * (sa - ea));
                    server.ratings.set(b.id, rb + 16 * (1 - sa - (1 - ea)));
                }
        for (const p of ps) server.ladder.add(String(p.id), server.ratings.get(p.id));
        for (const p of ps)
            p.client.ws.send(
                JSON.stringify({
                    type: "end",
                    kills: p.kills,
                    rating: Math.round(server.ratings.get(p.id)),
                    rank: server.ladder.revRank(String(p.id)) + 1,
                }),
            );
    }
}
function matchmake(now) {
    const q_ = [...server.queue].sort((a, b) => a.rating - b.rating);
    for (let i = 0; i + 4 <= q_.length; i++) {
        const group = q_.slice(i, i + 4),
            spread = group[3].rating - group[0].rating;
        const waited = Math.min(...group.map((c) => now - c.queuedAt));
        if (spread <= 150 + waited / 10) {
            server.queue = server.queue.filter((c) => !group.includes(c));
            const m = new Match(`m${server.matches.length + 1}`, group);
            server.matches.push(m);
            m.run();
            return matchmake(now);
        }
    }
}
const app = new App({ port: 0 });
let nextId = 1;
app.ws("/play", {
    open: () => {},
    message: (ws, data) => {
        const m = JSON.parse(data);
        if (m.type === "queue") {
            const c = { id: nextId++, ws, rating: m.rating, queuedAt: Date.now() };
            server.clients.set(ws, c);
            server.ratings.set(c.id, m.rating);
            server.queue.push(c);
            ws.send(JSON.stringify({ type: "queued", id: c.id }));
            matchmake(Date.now());
            return;
        }
        const c = server.clients.get(ws),
            p = c?.match?.players.get(c.id);
        if (!p) return;
        if (m.type === "input") p.inputs.push(m);
        if (m.type === "ack") p.acked = m.tick;
    },
});
app.start();

// =============================================================================
// Bot client with prediction and reconciliation
// =============================================================================
class Bot {
    constructor(name, rating, { latency = 0, cheat = false } = {}) {
        this.name = name;
        this.rating = rating;
        this.latency = latency;
        this.cheat = cheat;
        this.pending = [];
        this.seq = 0;
        this.errors = [];
        this.state = null;
        this.serverTick = 0;
        this.done = null;
    }
    async play() {
        const r = new Random(this.rating);
        await new Promise((resolve) => {
            this.done = resolve;
            this.ws = new WsClient(`ws://127.0.0.1:${app.port}/play`, {
                open: (ws) => ws.send(JSON.stringify({ type: "queue", rating: this.rating })),
                message: (ws, data) => setTimeout(() => this.onMessage(JSON.parse(data), r), this.latency / 2),
                close: () => {
                    this.closed = true;
                    clearInterval(this.loop);
                    resolve();
                },
            });
        });
    }
    send(obj) {
        setTimeout(() => {
            if (!this.closed) this.ws.send(JSON.stringify(obj));
        }, this.latency / 2);
    }
    onMessage(m, r) {
        if (m.type === "queued") this.id = m.id;
        if (m.type === "start") {
            this.me = { x: m.x, y: m.y };
            this.match = m.match;
            this.loop = setInterval(() => {
                // the client's own 20 Hz input loop
                const target = this.state
                    ? [...this.state.entries()].find(([id]) => id !== this.id && id < PACK_ID)
                    : null;
                let dx = r.nextFloat() * 2 - 1,
                    dy = r.nextFloat() * 2 - 1,
                    ax = 1,
                    ay = 0,
                    fire = false;
                if (target) {
                    const tx = target[1][0] / 16 - this.me.x,
                        ty = target[1][1] / 16 - this.me.y,
                        d = Math.hypot(tx, ty) || 1;
                    ax = tx / d;
                    ay = ty / d;
                    fire = d < 30 && r.nextFloat() < 0.4;
                    dx = ax * 0.5 + dx * 0.5;
                    dy = ay * 0.5 + dy * 0.5;
                }
                const len = Math.hypot(dx, dy);
                if (len > 1) {
                    dx /= len;
                    dy /= len;
                } // honest clients never exceed unit input
                if (this.cheat) {
                    dx *= 3;
                    dy *= 3;
                }
                const input = { type: "input", seq: ++this.seq, dx, dy, ax, ay, fire, viewTick: this.serverTick };
                step(this.me, input);
                this.pending.push(input);
                this.send(input);
            }, TICK_MS);
        }
        if (m.type === "snap") {
            const buf = Uint8Array.from(m.data),
                base = this.snaps?.get(decodeSnapshot(buf, null).baseTick);
            const snap = decodeSnapshot(buf, base);
            this.snaps ??= new Map();
            this.snaps.set(snap.tick, snap.state);
            for (const t of this.snaps.keys()) if (t < snap.tick - 40) this.snaps.delete(t);
            this.state = snap.state;
            this.serverTick = snap.tick;
            this.send({ type: "ack", tick: snap.tick });
            const mine = snap.state.get(this.id);
            if (mine) {
                // reconcile: server state + replay of unacked inputs
                const predicted = { ...this.me };
                this.me = { x: mine[0] / 16, y: mine[1] / 16 };
                this.pending = this.pending.filter((i) => i.seq > m.lastSeq);
                for (const i of this.pending) step(this.me, i);
                const err = Math.hypot(predicted.x - this.me.x, predicted.y - this.me.y);
                if (err > 5) this.respawns = (this.respawns || 0) + 1;
                else this.errors.push(err); // a respawn teleports: not a prediction error
            }
        }
        if (m.type === "end" || m.type === "kicked") {
            clearInterval(this.loop);
            this.result = m;
            this.closed = true;
            this.ws.close();
            this.done();
        }
    }
}

// =============================================================================
// Play
// =============================================================================
const results = {};
const bots = [
    new Bot("ace", 1810),
    new Bot("blaze", 1790, { latency: 120 }),
    new Bot("cobalt", 1760),
    new Bot("dusk", 1840, { cheat: true }),
    new Bot("ember", 1210),
    new Bot("frost", 1250, { latency: 60 }),
    new Bot("gale", 1190),
    new Bot("haze", 1230),
];
const t0 = Date.now();
await Promise.all(bots.map((b) => b.play()));
const elapsed = Date.now() - t0;
const [m1, m2] = server.matches;
const ids = (m) => [...bots.filter((b) => b.match === m.id).map((b) => b.name)].sort();
results["the matchmaker groups players of similar rating"] =
    server.matches.length === 2 &&
    ids(m1).every((n) => ["ace", "blaze", "cobalt", "dusk"].includes(n)) &&
    ids(m2).every((n) => ["ember", "frost", "gale", "haze"].includes(n));
results[
    `delta compression: ${server.stats.deltaBytes} bytes vs ${server.stats.fullBytes} for full snapshots (${((server.stats.deltaBytes / server.stats.fullBytes) * 100).toFixed(0)}%)`
] = server.stats.deltaBytes < server.stats.fullBytes * 0.7;
const honest = bots.filter((b) => !b.cheat && b.errors.length);
results[
    `prediction matches the server (median error ${honest.map((b) => stats.median(b.errors).toFixed(3)).join(", ")})`
] = honest.every((b) => stats.median(b.errors) < 0.05);
const cheater = bots.find((b) => b.cheat);
results[`the speed hacker was clamped ${server.stats.clamped} times and kicked`] =
    server.stats.clamped > 0 && cheater.result?.type === "kicked";
results[`${server.stats.hits} hits, ${server.stats.lagCompHits} of them only possible with lag compensation`] =
    server.stats.hits > 0 && server.stats.lagCompHits > 0;
const finished = bots.filter((b) => b.result?.type === "end");
results[`players picked up ${server.stats.pickups} health packs found through the KDTree`] = server.stats.pickups > 0;
results["the KDTree interest set always matched a brute-force scan"] = server.stats.interestMismatches === 0;
results["the ladder ranks the best final rating first"] =
    finished.find((b) => b.result.rank === 1)?.result.rating === Math.max(...finished.map((b) => b.result.rating));
results["Elo updates are zero-sum (up to rounding and the kicked player)"] =
    finished.length === 7 &&
    Math.abs(
        finished.reduce((s, b) => s + b.result.rating, 0) -
            [1810, 1790, 1760, 1210, 1250, 1190, 1230].reduce((a, b) => a + b, 0),
    ) <= 7;

print("multiplayer-game");
print(
    `  matches    ${server.matches.length} x 120 ticks @ ${1000 / TICK_MS} Hz in ${elapsed} ms wall; ${server.stats.snapshots} snapshots`,
);
print(
    table([
        ["player", "rating in", "latency", "match", "kills", "rating out", "pred. error p50/p99"],
        ...bots.map((b) => [
            b.name + (b.cheat ? " (cheat)" : ""),
            String(b.rating),
            `${b.latency} ms`,
            b.match,
            String(b.result?.kills ?? "-"),
            String(b.result?.rating ?? b.result?.why ?? "-"),
            b.errors.length
                ? `${stats.median(b.errors).toFixed(3)} / ${stats.quantile(b.errors, 0.99).toFixed(3)}`
                : "-",
        ]),
    ]),
);
print(
    `  pickups    ${server.stats.pickups} health packs collected; ladder ${server.ladder
        .range(0, 2, { reverse: true })
        .map(([id, r]) => `#${id} ${Math.round(r)}`)
        .join(", ")}`,
);
print(
    `  network    snapshots ${(server.stats.deltaBytes / 1024).toFixed(1)} KiB delta-encoded vs ${(server.stats.fullBytes / 1024).toFixed(1)} KiB full; kills ${server.matches.reduce((s, m) => s + m.events.length, 0)}`,
);
app.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
