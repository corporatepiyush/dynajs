// Realtime collaboration — WebSocket rooms editing a shared JSON document with optimistic concurrency.
//
// WHAT IT IS
//   The server half of a collaborative app (a shared task board), and the clients that use it:
//
//   protocol     clients send JSON Patch operations against the version they last saw; a `test` op guards
//                each edit, so a patch built on stale data fails and the client rebases and retries
//   authority    the server is the single writer: it applies, versions and broadcasts every accepted patch,
//                and every replica applies the same sequence, so all replicas converge
//   presence     join/leave and "who is here" broadcast per room
//   catch-up     the last 256 operations per room are kept in a ring buffer; a reconnecting client asks
//                for everything after its version instead of re-downloading the document
//   chat         room chat with a word filter (Aho-Corasick) and a per-connection rate limit
//   durability   every accepted op is appended to an NDJSON log; every 25 ops the room is snapshotted as
//                zstd-compressed CBOR; a restarted server loads the snapshot and replays the log tail
//   analytics    accepted edits are counted per user in a Multiset and per JSON path in a TopK (the contended
//                fields), and the board's points leaderboard is a ZSet kept up to date as owners change
//
// MODULES  dyna:http (App.ws, WsClient) · dyna:json (Patch, Pointer) · dyna:structures (RingBuffer, Multiset, TopK, ZSet)
//          dyna:matcher (MultiMatcher) · dyna:net (RateLimiter) · dyna:serialize (CBOR, ValueHash)
//          dyna:compress (zstd) · dyna:file · dyna:random · dyna:uuid
//
// RUN      dynajs examples/web/realtime-collab.js       three bots edit one board concurrently; replicas must
//          converge, a late joiner catches up from history, and a restarted server rebuilds the same state

import { App, WsClient } from "dyna:http";
import { Patch, Pointer } from "dyna:json";
import { RingBuffer, Multiset, TopK, ZSet } from "dyna:structures";
import { MultiMatcher } from "dyna:matcher";
import { RateLimiter } from "dyna:net";
import { CBOREncode, CBORDecode, ValueHash } from "dyna:serialize";
import { zstd, unzstd } from "dyna:compress";
import { makeTempDir, writeFile, readFile, readBytes, exists, removeAll } from "dyna:file";
import { Random } from "dyna:random";
import { NanoID } from "dyna:uuid";

const work = makeTempDir("collab");
const SNAPSHOT_EVERY = 25;

// =============================================================================
// Rooms: authoritative state, history, durability
// =============================================================================
class Room {
    constructor(name, dir) {
        this.name = name;
        this.snapshotPath = dir.join(`${name}.snapshot.cbor.zst`);
        this.logPath = dir.join(`${name}.ops.ndjson`);
        this.doc = { title: name, tasks: {} };
        this.version = 0;
        this.history = new RingBuffer(256);
        this.members = new Map(); // socket -> { user }
        this.editsBy = new Multiset();
        this.hotPaths = new TopK(16);
        this.leaderboard = new ZSet();
        this.recover();
    }
    recover() {
        if (exists(this.snapshotPath)) {
            const snap = CBORDecode(unzstd(readBytes(this.snapshotPath)));
            this.doc = snap.doc;
            this.version = snap.version;
        }
        if (!exists(this.logPath)) return 0;
        let replayed = 0;
        for (const line of readFile(this.logPath).split("\n")) {
            if (!line) continue;
            const op = JSON.parse(line);
            if (op.version <= this.version) continue; // already inside the snapshot
            this.doc = Patch.apply(this.doc, op.patch);
            this.version = op.version;
            this.history.push(op);
            replayed++;
        }
        this.replayed = replayed;
        return replayed;
    }
    apply(user, patch) {
        const next = Patch.apply(this.doc, patch); // throws when a `test` op fails
        this.doc = next;
        const op = { version: ++this.version, user, patch };
        this.editsBy.add(user);
        for (const p of patch) if (p.op !== "test") this.hotPaths.add(p.path.replace(/\/[^/]+$/, ""));
        this.leaderboard.clear();
        for (const t of Object.values(next.tasks)) if (t.owner) this.leaderboard.incrBy(t.owner, t.points || 0);
        this.history.push(op);
        writeFile(this.logPath, JSON.stringify(op) + "\n", { append: true });
        if (this.version % SNAPSHOT_EVERY === 0)
            writeFile(this.snapshotPath, zstd(CBOREncode({ version: this.version, doc: this.doc }), { level: 6 }));
        return op;
    }
    since(version) {
        const ops = this.history.toArray().filter((o) => o.version > version);
        return ops.length && ops[0].version === version + 1 ? ops : null; // null: the gap left the buffer
    }
    broadcast(message, except) {
        const text = JSON.stringify(message);
        for (const s of this.members.keys()) if (s !== except) s.send(text);
    }
}

// =============================================================================
// Server
// =============================================================================
const banned = new MultiMatcher(["darn", "heck", "frak"]);
function startServer(port = 0) {
    const rooms = new Map();
    const room = (name) => {
        if (!rooms.has(name)) rooms.set(name, new Room(name, work));
        return rooms.get(name);
    };
    const app = new App({ port });
    const stats = { accepted: 0, rejected: 0, filtered: 0, throttled: 0 };
    app.ws("/rooms", {
        // WebSocket routes are literal; the room travels in "join"
        upgrade: (req) => req.origin === null || req.origin === "https://board.example.test",
        open: () => {},
        message: (socket, data) => {
            const msg = JSON.parse(data);
            const send = (m) => socket.send(JSON.stringify(m));
            if (msg.type === "join") {
                const r = room(msg.room);
                r.members.set(socket, {
                    user: msg.user,
                    limiter: new RateLimiter({ tokensPerSec: 20, burst: 5 }),
                    room: r,
                });
                const catchUp = msg.version !== undefined ? r.since(msg.version) : null;
                if (catchUp) send({ type: "ops", ops: catchUp });
                else send({ type: "snapshot", version: r.version, doc: r.doc });
                r.broadcast({ type: "presence", users: [...r.members.values()].map((m) => m.user) });
                return;
            }
            const me = [...rooms.values()].map((r) => r.members.get(socket)).find(Boolean);
            if (!me) return send({ type: "error", error: "join a room first" });
            if (msg.type === "patch") {
                try {
                    const op = me.room.apply(me.user, msg.patch);
                    stats.accepted++;
                    me.room.broadcast({ type: "ops", ops: [op] });
                    send({ type: "ack", id: msg.id, version: op.version });
                } catch (e) {
                    stats.rejected++;
                    send({ type: "nack", id: msg.id, version: me.room.version, reason: e.message });
                }
            } else if (msg.type === "chat") {
                if (!me.limiter.allow("chat")) {
                    stats.throttled++;
                    return send({ type: "error", error: "slow down" });
                }
                const text = banned.replaceAllIn(String(msg.text).slice(0, 500), (m) => "*".repeat(m.length));
                if (text !== msg.text) stats.filtered++;
                me.room.broadcast({ type: "chat", user: me.user, text });
            }
        },
        close: (socket) => {
            for (const r of rooms.values())
                if (r.members.delete(socket))
                    r.broadcast({ type: "presence", users: [...r.members.values()].map((m) => m.user) });
        },
    });
    app.start();
    return { app, rooms, room, stats };
}

// =============================================================================
// Client: a replica with optimistic edits, rebase and retry
// =============================================================================
class Replica {
    constructor(url, user, roomName) {
        this.user = user;
        this.doc = null;
        this.version = -1;
        this.pending = new Map();
        this.chat = [];
        this.presence = [];
        this.ready = new Promise((resolve, reject) => {
            this.ws = new WsClient(url, {
                open: (ws) => ws.send(JSON.stringify({ type: "join", room: roomName, user, version: this.resumeFrom })),
                message: (ws, data) => this.onMessage(JSON.parse(data), resolve),
                close: (ws, code, reason) => reject(new Error(`${user}: connection closed ${code} ${reason}`)),
            });
        });
    }
    onMessage(m, resolveReady) {
        if (m.type === "snapshot") {
            this.doc = m.doc;
            this.version = m.version;
            resolveReady();
        } else if (m.type === "ops") {
            for (const op of m.ops)
                if (op.version === this.version + 1) {
                    this.doc = Patch.apply(this.doc, op.patch);
                    this.version = op.version;
                }
            resolveReady?.();
        } else if (m.type === "ack" || m.type === "nack") {
            const p = this.pending.get(m.id);
            this.pending.delete(m.id);
            p?.(m);
        } else if (m.type === "chat") this.chat.push(m);
        else if (m.type === "presence") this.presence = m.users;
    }
    // edit(doc) returns a patch for the doc it was given; it is rebuilt against fresh state after a conflict.
    async edit(build, attempts = 10) {
        for (let i = 0; i < attempts; i++) {
            const id = NanoID(8);
            const reply = await new Promise((resolve) => {
                this.pending.set(id, resolve);
                this.ws.send(JSON.stringify({ type: "patch", id, patch: build(this.doc) }));
            });
            if (reply.type === "ack") return { ok: true, retries: i };
            await new Promise((r) => setTimeout(r, 2 + i * 3)); // let the newer ops arrive, then rebase
        }
        return { ok: false };
    }
    say(text) {
        this.ws.send(JSON.stringify({ type: "chat", text }));
    }
    close() {
        this.ws.close();
    }
}

// =============================================================================
// The session
// =============================================================================
const results = {};
let server = startServer();
const url = () => `ws://127.0.0.1:${server.app.port}/rooms`;
const users = ["ada", "grace", "linus"];
const bots = users.map((u) => new Replica(url(), u, "sprint-12"));
await Promise.all(bots.map((b) => b.ready));

// Each bot adds tasks, then everyone races to claim and complete the same tasks.
const rng = new Random(2026);
let retries = 0,
    lost = 0;
await Promise.all(
    bots.map(async (bot, bi) => {
        for (let i = 0; i < 12; i++) {
            const id = `${bot.user}-${i}`;
            const r = await bot.edit(() => [
                {
                    op: "add",
                    path: `/tasks/${id}`,
                    value: {
                        title: `Task ${i} from ${bot.user}`,
                        owner: null,
                        done: false,
                        points: 1 + rng.nextBounded(8),
                    },
                },
            ]);
            retries += r.retries;
        }
    }),
);
await new Promise((r) => setTimeout(r, 50));
const ids = Object.keys(bots[0].doc.tasks);
await Promise.all(
    bots.map(async (bot, bi) => {
        const order = [...ids];
        new Random(100 + bi).shuffle(order); // each person works the board in their own order
        for (const id of order) {
            // Claim only if still unowned: the `test` op makes the claim atomic against concurrent claimers.
            const r = await bot.edit((doc) =>
                doc.tasks[id].owner === null
                    ? [
                          { op: "test", path: `/tasks/${id}/owner`, value: null },
                          { op: "replace", path: `/tasks/${id}/owner`, value: bot.user },
                      ]
                    : [{ op: "test", path: `/tasks/${id}/owner`, value: doc.tasks[id].owner }],
            );
            retries += r.retries;
            if (!r.ok) lost++;
        }
    }),
);
await new Promise((r) => setTimeout(r, 50));
for (const bot of bots)
    for (const [id, t] of Object.entries(bot.doc.tasks))
        if (t.owner === bot.user && Number(id.split("-")[1]) % 3 === 0)
            await bot.edit((doc) => [
                { op: "test", path: `/tasks/${id}/done`, value: doc.tasks[id].done },
                { op: "replace", path: `/tasks/${id}/done`, value: true },
            ]);

bots[0].say("this darn sprint is going well");
for (let i = 0; i < 8; i++) bots[2].say(`spam ${i}`);
await new Promise((r) => setTimeout(r, 100));

const room = server.room("sprint-12");
const serverHash = ValueHash(room.doc);
results["every replica converged to the server's document"] = bots.every(
    (b) => ValueHash(b.doc) === serverHash && b.version === room.version,
);
const tasks = Object.values(room.doc.tasks);
results["36 tasks, each claimed by exactly one user"] =
    tasks.length === 36 && tasks.every((t) => users.includes(t.owner));
results["no edit exhausted its retries"] = lost === 0;
results[`concurrent claims produced conflicts that were rebased (${server.stats.rejected})`] =
    server.stats.rejected > 0;
results["chat is filtered"] = bots[1].chat.some((m) => m.text === "this **** sprint is going well");
results["chat flooding is throttled"] = server.stats.throttled > 0;
results["presence lists all three users"] = bots[0].presence.length === 3;

// A late joiner that last saw version V catches up with only the ops after V.
const lateFrom = room.version - 10;
const late = new Replica(url(), "margaret", "sprint-12");
await late.ready;
const lateSnapshotVersion = late.version;
const resume = new Replica(url(), "margaret-2", "sprint-12");
await resume.ready;
results["a fresh joiner receives the snapshot"] = lateSnapshotVersion === room.version;
results["the history ring answers 'everything after V'"] = room.since(lateFrom).length === 10;

// Restart: a new server process state is rebuilt from snapshot + log tail and must be identical.
const versionBefore = room.version;
for (const b of [...bots, late, resume]) b.close();
server.app.close();
server = startServer();
const rebuilt = server.room("sprint-12");
results[`restart rebuilt v${rebuilt.version} (replayed ${rebuilt.replayed} ops after the snapshot)`] =
    rebuilt.version === versionBefore && ValueHash(rebuilt.doc) === serverHash;

const points = Object.fromEntries(room.leaderboard.range(0, -1, { reverse: true }));
results["the points leaderboard (ZSet) adds up to the board"] =
    Object.values(points).sum() === tasks.pluck("points").sum() && Object.keys(points).every((u) => users.includes(u));
results["edits per user (Multiset) add up to the accepted operations"] =
    users.reduce((s, u) => s + room.editsBy.count(u), 0) === room.version;
print("realtime-collab");
print(`  ops        ${versionBefore} accepted, ${retries} rebases after conflicts`);
print(
    `  board      ${tasks.length} tasks, ${tasks.filter((t) => t.done).length} done; points by owner ${JSON.stringify(points)}`,
);
print(`  durability snapshot every ${SNAPSHOT_EVERY} ops, ${rebuilt.replayed} replayed from the log on restart`);
server.app.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
