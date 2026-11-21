// Redis-compatible store — a RESP2 server with strings, counters, expiry, lists, hashes, sorted sets, pub/sub,
// MULTI/EXEC transactions, pipelining, an append-only file with rewrite, and LRU eviction under maxmemory,
// exercised through the engine's own Redis CLIENT so the protocol is checked by an independent implementation.
//
// WHAT IT IS
//   protocol     RESP2 parsing of arrays of bulk strings (and inline commands), replies as simple strings,
//                errors, integers, bulk strings and arrays; partial frames across TCP reads are buffered,
//                many commands in one read are answered in order (pipelining) and their replies go out in one write
//   data types   strings (GET/SET with EX/PX/NX/XX, INCR/INCRBY, APPEND, MGET), keys (DEL, EXISTS, EXPIRE,
//                TTL, PERSIST, KEYS with globs, TYPE), lists (LPUSH/RPUSH/LPOP/RPOP/LRANGE/LLEN), hashes
//                (HSET/HGET/HGETALL/HDEL/HINCRBY), sorted sets (ZADD/ZSCORE/ZRANGE … WITHSCORES/ZRANGEBYSCORE/
//                ZRANK/ZREM, ordered by score then member)
//   blocking     BLPOP/BRPOP park the client: waiters are served oldest first after the push that fed them, a parked
//                client's later pipelined commands wait behind it, a timeout answers nil, and inside MULTI it never
//                blocks; a served pop is logged as LPOP/RPOP after the push, so the AOF replays it
//   expiry       lazy on access plus active sampling of keys with a TTL, as Redis does
//   pub/sub      SUBSCRIBE/PUBLISH with the pushed "message" arrays
//   transactions MULTI queues, EXEC runs atomically, WATCH aborts EXEC if a watched key changed
//   persistence  every write is appended to an AOF in RESP through a buffered FileWriter flushed once per read
//                batch, BEFORE that batch's replies are written (Redis's beforeSleep order); BGREWRITEAOF compacts it
//                to the minimal command set; a restarted server replays it and has the same data
//   oracles      4000 random commands (strings, lists, hashes, sorted sets, ~650 of them WRONGTYPE) must answer
//                exactly as a plain model that knows Redis's rules but none of this server's code, and the keyspace
//                must equal the model live, after replaying the AOF, and after a rewrite and second restart; the RESP
//                parser must yield the same commands when its input arrives in random 1-40-byte pieces
//   eviction     maxmemory with allkeys-lru: approximate memory accounting and eviction of the least recently
//                used keys when a write would exceed the limit
//   internals    the way Redis itself is built, on native structures: a list is a Deque (O(1) push/pop at both
//                ends, not an array copied per write), a sorted set is a ZSet (ZRANGE/ZRANK/ZRANGEBYSCORE without
//                sorting per query), PFADD/PFCOUNT is a HyperLogLog, keys are interned to ids so expiry is a min
//                IndexedHeap of deadlines (popUntil(now) removes exactly the expired keys, no sampling) and LRU is a
//                min IndexedHeap of last-use ticks (eviction pops the true least recently used key)
//
// MODULES  dyna:net (TCPServer) · dyna:redis · dyna:file · dyna:random · dyna:async (sleep) · dyna:cli (Table)
//          · dyna:structures (Deque, ZSet, HyperLogLog, Interner, IndexedHeap) · dyna:encoding (base64)
//
// RUN      dynajs examples/net/redis-compatible-store.js     starts the server and drives it with dyna:redis's Redis client

import { TCPServer } from "dyna:net";
import { Redis } from "dyna:redis";
import { makeTempDir, writeFile, readFile, exists, stat, removeAll, FileWriter } from "dyna:file";
import { Random } from "dyna:random";
import { sleep } from "dyna:async";
import { Table } from "dyna:cli";
import { Deque, ZSet, HyperLogLog, Interner, IndexedHeap } from "dyna:structures";
import { Base64Encode, Base64Decode } from "dyna:encoding";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("redis");
const rng = new Random(6379);
const enc = new TextEncoder();

// =============================================================================
// RESP
// =============================================================================
const resp = {
    ok: () => "+OK\r\n",
    simple: (s) => `+${s}\r\n`,
    err: (s) => `-${s}\r\n`,
    int: (n) => `:${n}\r\n`,
    nil: () => "$-1\r\n",
    nilArr: () => "*-1\r\n",
    bulk: (s) => (s === null || s === undefined ? "$-1\r\n" : `$${enc.encode(String(s)).length}\r\n${s}\r\n`),
    arr: (xs) =>
        `*${xs.length}\r\n${xs.map((x) => (Array.isArray(x) ? resp.arr(x) : typeof x === "number" ? resp.int(x) : resp.bulk(x))).join("")}`,
};
function parser(onCommand) {
    // works on bytes: lengths in RESP are byte counts
    let buf = new Uint8Array(0);
    const dec = new TextDecoder();
    const lineEnd = (from) => {
        for (let i = from; i + 1 < buf.length; i++) if (buf[i] === 13 && buf[i + 1] === 10) return i;
        return -1;
    };
    return (chunk) => {
        const bytes = typeof chunk === "string" ? enc.encode(chunk) : chunk;
        const merged = new Uint8Array(buf.length + bytes.length);
        merged.set(buf);
        merged.set(bytes, buf.length);
        buf = merged;
        let start = 0;
        for (;;) {
            if (start >= buf.length) break;
            if (buf[start] !== 42) {
                // inline command
                const nl = lineEnd(start);
                if (nl < 0) break;
                const line = dec.decode(buf.subarray(start, nl)).trim();
                start = nl + 2;
                if (line) onCommand(line.split(/\s+/));
                continue;
            }
            const hdr = lineEnd(start);
            if (hdr < 0) break;
            const n = Number(dec.decode(buf.subarray(start + 1, hdr))),
                args = [];
            let at = hdr + 2,
                complete = true;
            for (let i = 0; i < n; i++) {
                const e = lineEnd(at);
                if (e < 0 || buf[at] !== 36) {
                    complete = false;
                    break;
                }
                const len = Number(dec.decode(buf.subarray(at + 1, e)));
                if (buf.length < e + 2 + len + 2) {
                    complete = false;
                    break;
                }
                args.push(dec.decode(buf.subarray(e + 2, e + 2 + len)));
                at = e + 2 + len + 2;
            }
            if (!complete) break;
            start = at;
            onCommand(args);
        }
        buf = buf.slice(start);
    };
}

// =============================================================================
// Keyspace
// =============================================================================
const WRITES = new Set([
    "SET",
    "DEL",
    "INCR",
    "INCRBY",
    "APPEND",
    "EXPIRE",
    "PEXPIREAT",
    "PERSIST",
    "LPUSH",
    "RPUSH",
    "LPOP",
    "RPOP",
    "HSET",
    "HDEL",
    "HINCRBY",
    "ZADD",
    "ZINCRBY",
    "ZREM",
    "PFADD",
    "PFRESTORE",
    "FLUSHALL",
]);
const BLOCKED = Symbol("blocked"); // a command that parked its client instead of replying

class Store {
    constructor({ aofPath, maxmemory = Infinity }) {
        this.data = new Map();
        this.expires = new Map();
        this.versions = new Map();
        this.channels = new Map();
        this.aofPath = aofPath;
        this.maxmemory = maxmemory;
        this.used = 0;
        this.tick = 0;
        this.stats = { commands: 0, evicted: 0, expired: 0, hits: 0, misses: 0 };
        this.ids = new Interner();
        this.deadlines = new IndexedHeap();
        this.lru = new IndexedHeap();
        this.replaying = false;
        this.waiters = new Map(); // list key -> clients blocked on it, in arrival order
        this.ready = new Set(); // keys pushed to while someone waits on them
        this.dirty = new Set(); // clients with replies waiting for the end of the batch
        this.depth = 0;
        this.serving = false;
        if (aofPath && exists(aofPath)) this.replay(readFile(aofPath));
        this.aof = aofPath ? new FileWriter(aofPath, { append: true }) : null;
    }
    close() {
        this.aof?.close();
    }
    // Replies are buffered per client and written once per batch, AFTER the AOF buffer is flushed: what a client has
    // been told is never newer than what a restart would replay (Redis does the same in beforeSleep).
    reply(client, out) {
        if (!client.out.length) this.dirty.add(client);
        client.out.push(out);
    }
    drain() {
        this.aof?.flush();
        for (const c of this.dirty) {
            if (!c.closed) c.conn.write(c.out.join(""));
            c.out = [];
        }
        this.dirty.clear();
    }
    submit(client, args) {
        if (client.blocked) return void client.pending.push(args); // a blocked client's pipeline waits behind it
        const out = this.exec(args, client);
        if (out !== BLOCKED) this.reply(client, out);
    }
    unblock(client, out) {
        const w = client.blocked;
        if (!w) return;
        clearTimeout(w.timer);
        for (const k of w.keys) {
            const q = this.waiters.get(k),
                i = q ? q.indexOf(w) : -1;
            if (i >= 0) q.splice(i, 1);
            if (q && !q.length) this.waiters.delete(k);
        }
        client.blocked = null;
        if (out === null) return; // the connection went away
        this.reply(client, out);
        while (!client.blocked && client.pending.length) this.submit(client, client.pending.shift());
    }
    // After a top-level command, hand pushed elements to blocked clients, oldest first; each pop is logged AFTER the
    // push that fed it, so a replay reproduces the list.
    serveReady() {
        if (this.serving) return;
        this.serving = true;
        for (const key of this.ready) {
            this.ready.delete(key);
            const q = this.waiters.get(key) ?? [];
            while (q.length) {
                const list = this.data.get(key);
                if (!list || list.type !== "list" || !list.value.length) break;
                const w = q.shift();
                const x = w.side === "L" ? list.value.popFront() : list.value.popBack();
                list.value.length ? this.set(key, list) : this.remove(key);
                this.log([w.side === "L" ? "LPOP" : "RPOP", key]);
                this.unblock(w.client, resp.arr([key, x]));
            }
        }
        this.serving = false;
    }
    replay(text) {
        this.replaying = true;
        const p = parser((args) => this.exec(args, null));
        p(text);
        this.replaying = false;
    }
    sizeOf(key, v) {
        const body =
            v.type === "string"
                ? v.value.length
                : v.type === "hash"
                  ? [...v.value].reduce((s, [k, x]) => s + k.length + String(x).length + 16, 0)
                  : v.type === "list"
                    ? v.value.toArray().reduce((s, x) => s + x.length + 8, 0)
                    : v.type === "zset"
                      ? v.value.size * 48
                      : 12 * 1024; // a HyperLogLog register file
        return key.length + body + 48;
    }
    touch(key) {
        this.lru.push(this.ids.intern(key), ++this.tick);
    }
    expireAt(key, at) {
        this.expires.set(key, at);
        this.deadlines.push(this.ids.intern(key), at);
    }
    persist(key) {
        const id = this.ids.lookup(key);
        if (id !== undefined) this.deadlines.delete(id);
        return this.expires.delete(key);
    }
    alive(key) {
        const e = this.expires.get(key);
        if (e !== undefined && e <= Date.now()) {
            this.remove(key);
            this.stats.expired++;
            return false;
        }
        return this.data.has(key);
    }
    get(key, type) {
        if (!this.alive(key)) {
            this.stats.misses++;
            return undefined;
        }
        const v = this.data.get(key);
        if (type && v.type !== type)
            throw new Error("WRONGTYPE Operation against a key holding the wrong kind of value");
        this.stats.hits++;
        this.touch(key);
        return v;
    }
    set(key, v) {
        // also called after an in-place mutation
        const old = this.data.get(key);
        if (old) this.used -= old.bytes;
        v.bytes = this.sizeOf(key, v);
        this.used += v.bytes;
        this.data.set(key, v);
        this.touch(key);
        this.versions.set(key, (this.versions.get(key) || 0) + 1);
        this.evict();
    }
    remove(key) {
        const old = this.data.get(key);
        if (!old) return false;
        this.used -= old.bytes;
        this.data.delete(key);
        this.persist(key);
        const id = this.ids.lookup(key);
        if (id !== undefined) this.lru.delete(id);
        this.versions.set(key, (this.versions.get(key) || 0) + 1);
        return true;
    }
    evict() {
        while (this.used > this.maxmemory && this.data.size > 1) {
            // the true least recently used key
            const victim = this.ids.get(this.lru.pop());
            if (!this.data.has(victim)) continue;
            this.remove(victim);
            this.stats.evicted++;
            this.log(["DEL", victim]);
        }
    }
    activeExpire() {
        // exactly the keys whose deadline passed
        for (const id of this.deadlines.popUntil(Date.now())) {
            const key = this.ids.get(id);
            if (this.data.has(key)) {
                this.remove(key);
                this.stats.expired++;
            }
        }
    }
    log(args) {
        if (!this.replaying && this.aof)
            this.aof.write(
                `*${args.length}\r\n${args.map((a) => `$${enc.encode(String(a)).length}\r\n${a}\r\n`).join("")}`,
            );
    }
    rewriteAof() {
        const out = [];
        for (const [k, v] of this.data) {
            if (!this.alive(k)) continue;
            if (v.type === "string") out.push(["SET", k, v.value]);
            if (v.type === "list" && v.value.length) out.push(["RPUSH", k, ...v.value.toArray()]);
            if (v.type === "hash") out.push(["HSET", k, ...[...v.value].flat()]);
            if (v.type === "zset") out.push(["ZADD", k, ...v.value.entries().flatMap(([m, s]) => [String(s), m])]);
            if (v.type === "hll") out.push(["PFRESTORE", k, Base64Encode(v.value.serialize())]);
            const e = this.expires.get(k);
            if (e) out.push(["PEXPIREAT", k, String(e)]);
        }
        this.aof.close();
        writeFile(
            this.aofPath,
            out
                .map((a) => `*${a.length}\r\n${a.map((x) => `$${enc.encode(String(x)).length}\r\n${x}\r\n`).join("")}`)
                .join(""),
        );
        this.aof = new FileWriter(this.aofPath, { append: true });
        return out.length;
    }
    // ----- commands -----
    exec(args, client) {
        this.depth++;
        let out;
        try {
            out = this.execOne(args, client);
        } finally {
            this.depth--;
        }
        if (this.depth === 0 && this.ready.size) this.serveReady();
        return out;
    }
    execOne(args, client) {
        this.stats.commands++;
        const cmd = args[0].toUpperCase(),
            a = args.slice(1);
        if (client?.multi && !["EXEC", "DISCARD", "MULTI", "WATCH"].includes(cmd)) {
            client.multi.push(args);
            return resp.simple("QUEUED");
        }
        try {
            const out = this.run(cmd, a, client);
            if (out === BLOCKED) return out;
            if (WRITES.has(cmd) && !out.startsWith("-"))
                this.log(cmd === "EXPIRE" ? ["PEXPIREAT", a[0], String(this.expires.get(a[0]) ?? 0)] : args);
            return out;
        } catch (e) {
            return resp.err(e.message.startsWith("WRONGTYPE") ? e.message : `ERR ${e.message}`);
        }
    }
    run(cmd, a, client) {
        const int = (s) => {
            if (!/^-?\d+$/.test(s)) throw new Error("value is not an integer or out of range");
            return Number(s);
        };
        switch (cmd) {
            case "PING":
                return a.length ? resp.bulk(a[0]) : resp.simple("PONG");
            case "SET": {
                let px = null,
                    nx = false,
                    xx = false;
                for (let i = 2; i < a.length; i++) {
                    const o = a[i].toUpperCase();
                    if (o === "EX") px = int(a[++i]) * 1000;
                    else if (o === "PX") px = int(a[++i]);
                    else if (o === "NX") nx = true;
                    else if (o === "XX") xx = true;
                    else throw new Error("syntax error");
                }
                const exists_ = this.alive(a[0]);
                if ((nx && exists_) || (xx && !exists_)) return resp.nil();
                this.set(a[0], { type: "string", value: a[1] });
                if (px !== null) this.expireAt(a[0], Date.now() + px);
                else this.persist(a[0]);
                return resp.ok();
            }
            case "GET":
                return resp.bulk(this.get(a[0], "string")?.value ?? null);
            case "MGET":
                return resp.arr(
                    a.map((k) => {
                        const v = this.alive(k) ? this.data.get(k) : null;
                        return v?.type === "string" ? v.value : null;
                    }),
                );
            case "INCR":
            case "INCRBY": {
                const cur = this.get(a[0], "string"),
                    n = cur ? int(cur.value) : 0,
                    by = cmd === "INCR" ? 1 : int(a[1]);
                this.set(a[0], { type: "string", value: String(n + by) });
                return resp.int(n + by);
            }
            case "APPEND": {
                const cur = this.get(a[0], "string")?.value ?? "";
                this.set(a[0], { type: "string", value: cur + a[1] });
                return resp.int(enc.encode(cur + a[1]).length); // Redis answers in bytes
            }
            case "DEL":
                return resp.int(a.filter((k) => this.alive(k) && this.remove(k)).length);
            case "EXISTS":
                return resp.int(a.filter((k) => this.alive(k)).length);
            case "TYPE":
                return resp.simple(
                    this.alive(a[0])
                        ? this.data.get(a[0]).type === "hll"
                            ? "string"
                            : this.data.get(a[0]).type
                        : "none",
                );
            case "EXPIRE":
                if (!this.alive(a[0])) return resp.int(0);
                this.expireAt(a[0], Date.now() + int(a[1]) * 1000);
                return resp.int(1);
            case "PEXPIREAT":
                if (!this.alive(a[0])) return resp.int(0);
                this.expireAt(a[0], int(a[1]));
                return resp.int(1);
            case "PERSIST":
                return resp.int(this.alive(a[0]) && this.persist(a[0]) ? 1 : 0);
            case "TTL": {
                if (!this.alive(a[0])) return resp.int(-2);
                const e = this.expires.get(a[0]);
                return resp.int(e === undefined ? -1 : Math.ceil((e - Date.now()) / 1000));
            }
            case "KEYS": {
                const re = new RegExp(
                    "^" +
                        a[0]
                            .replace(/[.+^${}()|\\]/g, "\\$&")
                            .replace(/\*/g, ".*")
                            .replace(/\?/g, ".") +
                        "$",
                );
                return resp.arr([...this.data.keys()].filter((k) => this.alive(k) && re.test(k)).sort());
            }
            case "LPUSH":
            case "RPUSH": {
                const v = this.get(a[0], "list") ?? { type: "list", value: new Deque() };
                for (const x of a.slice(1)) cmd === "LPUSH" ? v.value.pushFront(x) : v.value.pushBack(x);
                this.set(a[0], v);
                if (this.waiters.has(a[0])) this.ready.add(a[0]);
                return resp.int(v.value.length);
            }
            case "LPOP":
            case "RPOP": {
                const cur = this.get(a[0], "list");
                if (!cur || !cur.value.length) return resp.nil();
                const x = cmd === "LPOP" ? cur.value.popFront() : cur.value.popBack();
                cur.value.length ? this.set(a[0], cur) : this.remove(a[0]);
                return resp.bulk(x);
            }
            case "BLPOP":
            case "BRPOP": {
                const keys = a.slice(0, -1),
                    timeout = Number(a.at(-1)),
                    left = cmd === "BLPOP";
                if (!keys.length || !(timeout >= 0)) throw new Error("timeout is not a float or out of range");
                for (const k of keys) {
                    const l = this.get(k, "list");
                    if (!l?.value.length) continue;
                    const x = left ? l.value.popFront() : l.value.popBack();
                    l.value.length ? this.set(k, l) : this.remove(k);
                    this.log([left ? "LPOP" : "RPOP", k]);
                    return resp.arr([k, x]);
                }
                if (!client) return resp.nilArr(); // inside MULTI (and in a replay) a blocking pop never blocks
                const w = { client, keys, side: left ? "L" : "R", timer: 0 };
                for (const k of keys) this.waiters.getOrInsertComputed(k, () => []).push(w);
                client.blocked = w;
                if (timeout > 0)
                    w.timer = setTimeout(() => {
                        this.unblock(client, resp.nilArr());
                        this.drain();
                    }, timeout * 1000);
                return BLOCKED;
            }
            case "LLEN":
                return resp.int(this.get(a[0], "list")?.value.length ?? 0);
            case "LRANGE": {
                const l = this.get(a[0], "list")?.value,
                    n = l?.length ?? 0;
                const s = int(a[1]) < 0 ? Math.max(0, n + int(a[1])) : int(a[1]),
                    e = int(a[2]) < 0 ? n + int(a[2]) : Math.min(n - 1, int(a[2]));
                const out = [];
                for (let i = s; i <= e; i++) out.push(l.get(i));
                return resp.arr(out);
            }
            case "HSET": {
                const h = new Map(this.get(a[0], "hash")?.value ?? []);
                let added = 0;
                for (let i = 1; i + 1 < a.length; i += 2) {
                    if (!h.has(a[i])) added++;
                    h.set(a[i], a[i + 1]);
                }
                this.set(a[0], { type: "hash", value: h });
                return resp.int(added);
            }
            case "HGET":
                return resp.bulk(this.get(a[0], "hash")?.value.get(a[1]) ?? null);
            case "HGETALL":
                return resp.arr([...(this.get(a[0], "hash")?.value ?? [])].flat());
            case "HDEL": {
                const cur = this.get(a[0], "hash");
                if (!cur) return resp.int(0);
                const h = new Map(cur.value);
                const n = a.slice(1).filter((f) => h.delete(f)).length;
                h.size ? this.set(a[0], { type: "hash", value: h }) : this.remove(a[0]);
                return resp.int(n);
            }
            case "HINCRBY": {
                const h = new Map(this.get(a[0], "hash")?.value ?? []);
                const n = int(h.get(a[1]) ?? "0") + int(a[2]);
                h.set(a[1], String(n));
                this.set(a[0], { type: "hash", value: h });
                return resp.int(n);
            }
            case "ZADD":
            case "ZINCRBY": {
                const z = this.get(a[0], "zset") ?? { type: "zset", value: new ZSet() };
                let added = 0,
                    last = 0;
                for (let i = 1; i + 1 < a.length; i += 2) {
                    const sc = Number(a[i]);
                    if (Number.isNaN(sc)) throw new Error("value is not a valid float");
                    if (cmd === "ZINCRBY") last = z.value.incrBy(a[i + 1], sc);
                    else if (z.value.add(a[i + 1], sc)) added++;
                }
                this.set(a[0], z);
                return cmd === "ZINCRBY" ? resp.bulk(String(last)) : resp.int(added);
            }
            case "ZSCORE": {
                const s = this.get(a[0], "zset")?.value.score(a[1]);
                return resp.bulk(s === undefined ? null : String(s));
            }
            case "ZCARD":
                return resp.int(this.get(a[0], "zset")?.value.size ?? 0);
            case "ZREM": {
                const cur = this.get(a[0], "zset");
                if (!cur) return resp.int(0);
                const n = a.slice(1).filter((m) => cur.value.delete(m)).length;
                cur.value.size ? this.set(a[0], cur) : this.remove(a[0]);
                return resp.int(n);
            }
            case "ZRANK":
            case "ZREVRANK": {
                const z = this.get(a[0], "zset")?.value;
                const r = cmd === "ZRANK" ? z?.rank(a[1]) : z?.revRank(a[1]);
                return r === undefined ? resp.nil() : resp.int(r);
            }
            case "ZRANGE":
            case "ZREVRANGE":
            case "ZRANGEBYSCORE": {
                const z = this.get(a[0], "zset")?.value ?? new ZSet();
                const withScores = a.some((x) => x.toUpperCase() === "WITHSCORES");
                let sel;
                if (cmd !== "ZRANGEBYSCORE") sel = z.range(int(a[1]), int(a[2]), { reverse: cmd === "ZREVRANGE" });
                else {
                    const bound = (x) =>
                        x === "-inf"
                            ? [-Infinity, false]
                            : x === "+inf"
                              ? [Infinity, false]
                              : x.startsWith("(")
                                ? [Number(x.slice(1)), true]
                                : [Number(x), false];
                    const [lo, loOpen] = bound(a[1]),
                        [hi, hiOpen] = bound(a[2]);
                    sel = z.rangeByScore(lo, hi).filter(([, sc]) => !(loOpen && sc === lo) && !(hiOpen && sc === hi));
                }
                return resp.arr(withScores ? sel.flatMap(([m, s]) => [m, String(s)]) : sel.map(([m]) => m));
            }
            case "PFADD": {
                const h = this.get(a[0], "hll") ?? { type: "hll", value: new HyperLogLog(14) };
                const before = h.value.count();
                for (const x of a.slice(1)) h.value.add(x);
                this.set(a[0], h);
                return resp.int(h.value.count() !== before ? 1 : 0);
            }
            case "PFCOUNT":
                return resp.int(
                    Math.round(
                        a
                            .reduce((u, k) => {
                                const h = this.get(k, "hll");
                                return h ? u.merge(h.value) : u;
                            }, new HyperLogLog(14))
                            .count(),
                    ),
                );
            case "PFRESTORE":
                this.set(a[0], { type: "hll", value: HyperLogLog.deserialize(Base64Decode(a[1])) });
                return resp.ok();
            case "SUBSCRIBE": {
                for (const ch of a) {
                    if (!this.channels.has(ch)) this.channels.set(ch, new Set());
                    this.channels.get(ch).add(client);
                }
                return a.map((ch, i) => resp.arr(["subscribe", ch, i + 1])).join("");
            }
            case "PUBLISH": {
                const subs = this.channels.get(a[0]) || new Set();
                for (const c of subs) this.reply(c, resp.arr(["message", a[0], a[1]]));
                return resp.int(subs.size);
            }
            case "MULTI":
                client.multi = [];
                return resp.ok();
            case "DISCARD":
                client.multi = null;
                client.watch = null;
                return resp.ok();
            case "WATCH":
                client.watch = new Map(a.map((k) => [k, this.versions.get(k) || 0]));
                return resp.ok();
            case "EXEC": {
                const queued = client.multi;
                client.multi = null;
                if (!queued) throw new Error("EXEC without MULTI");
                const dirty = client.watch && [...client.watch].some(([k, v]) => (this.versions.get(k) || 0) !== v);
                client.watch = null;
                if (dirty) return resp.nilArr();
                return `*${queued.length}\r\n${queued.map((q) => this.exec(q, null)).join("")}`;
            }
            case "DBSIZE":
                return resp.int([...this.data.keys()].filter((k) => this.alive(k)).length);
            case "FLUSHALL":
                for (const k of [...this.data.keys()]) this.remove(k);
                return resp.ok();
            case "BGREWRITEAOF":
                return resp.simple(`Background append only file rewriting started (${this.rewriteAof()} commands)`);
            case "INFO":
                return resp.bulk(
                    `# Stats\r\ntotal_commands_processed:${this.stats.commands}\r\nevicted_keys:${this.stats.evicted}\r\nexpired_keys:${this.stats.expired}\r\nkeyspace_hits:${this.stats.hits}\r\nkeyspace_misses:${this.stats.misses}\r\nused_memory:${this.used}\r\n`,
                );
            case "HELLO":
                throw new Error("unknown command 'HELLO'"); // RESP2 only: clients fall back
            case "CLIENT":
            case "SELECT":
                return resp.ok();
            default:
                throw new Error(`unknown command '${cmd.toLowerCase()}'`);
        }
    }
}
function startServer(store) {
    const srv = new TCPServer({ port: 0 });
    srv.start({
        connect: (conn) => {
            const client = { conn, multi: null, watch: null, blocked: null, pending: [], out: [], closed: false };
            conn.feed = parser((args) => store.submit(client, args));
            conn.client = client;
        },
        data: (conn, bytes) => {
            conn.feed(bytes);
            store.drain();
        },
        close: (conn) => {
            for (const s of store.channels.values()) s.delete(conn.client);
            store.unblock(conn.client, null);
            conn.client.closed = true;
        },
    });
    const sweeper = setInterval(() => store.activeExpire(), 20);
    return {
        srv,
        stop: () => {
            clearInterval(sweeper);
            srv.close();
            store.drain();
            store.close();
        },
    };
}

// =============================================================================
// Drive it with the engine's Redis client
// =============================================================================
const results = {};
const aofPath = work.join("appendonly.aof");
let store = new Store({ aofPath, maxmemory: 256 * 1024 });
let server = startServer(store);
const r = new Redis({ host: "127.0.0.1", port: server.srv.port, commandTimeoutMs: 3000 });
const cmd = (...a) => r.command(...a);

results["PING, SET and GET round-trip UTF-8 values"] =
    (await cmd("PING")) === "PONG" &&
    (await cmd("SET", "greeting", "héllo wörld")) === "OK" &&
    (await cmd("GET", "greeting")) === "héllo wörld";
results["SET NX/XX respect existence"] =
    (await cmd("SET", "greeting", "x", "NX")) === null && (await cmd("SET", "missing", "x", "XX")) === null;
results["INCR/INCRBY"] = (await cmd("INCRBY", "visits", 41)) === 41 && (await cmd("INCR", "visits")) === 42;
let wrong = null;
try {
    await cmd("INCR", "greeting");
} catch (e) {
    wrong = String(e.message || e);
}
results["INCR on a non-number is an error reply"] = /not an integer/.test(wrong || "");
await cmd("SET", "session:1", "alice", "PX", 150);
results["TTL reports expiry and persistence"] =
    (await cmd("TTL", "session:1")) === 1 && (await cmd("TTL", "greeting")) === -1;
await cmd("RPUSH", "queue", "a", "b", "c");
await cmd("LPUSH", "queue", "z");
results["lists"] =
    JSON.stringify(await cmd("LRANGE", "queue", 0, -1)) === '["z","a","b","c"]' && (await cmd("RPOP", "queue")) === "c";
await cmd("HSET", "user:7", "name", "Ada", "plan", "pro");
await cmd("HINCRBY", "user:7", "logins", 3);
const h = await cmd("HGETALL", "user:7");
results["hashes"] = JSON.stringify(h) === '["name","Ada","plan","pro","logins","3"]' || (h && h.name === "Ada");
for (const [m, s] of [
    ["carol", 30],
    ["alice", 10],
    ["bob", 20],
    ["dave", 20],
])
    await cmd("ZADD", "leaderboard", s, m);
results["sorted sets order by score, then member"] =
    JSON.stringify(await cmd("ZRANGE", "leaderboard", 0, -1)) === '["alice","bob","dave","carol"]' &&
    (await cmd("ZRANK", "leaderboard", "carol")) === 3;
results["ZRANGEBYSCORE with an exclusive bound"] =
    JSON.stringify(await cmd("ZRANGEBYSCORE", "leaderboard", "(10", "+inf", "WITHSCORES")) ===
    '["bob","20","dave","20","carol","30"]';
await cmd("ZINCRBY", "leaderboard", 25, "alice");
results["ZINCRBY reorders; ZREVRANGE and ZREVRANK read from the top"] =
    JSON.stringify(await cmd("ZREVRANGE", "leaderboard", 0, 1)) === '["alice","carol"]' &&
    (await cmd("ZREVRANK", "leaderboard", "bob")) === 3 &&
    (await cmd("ZCARD", "leaderboard")) === 4;
await cmd("ZINCRBY", "leaderboard", -25, "alice");
for (let b = 0; b < 20; b++)
    await cmd("PFADD", "visitors", ...Array.from({ length: 500 }, (_, i) => `v${b * 500 + i}`));
await cmd("PFADD", "visitors-mobile", ...Array.from({ length: 2000 }, (_, i) => `v${i * 7}`));
const pf = await cmd("PFCOUNT", "visitors"),
    pfUnion = await cmd("PFCOUNT", "visitors", "visitors-mobile");
results[`PFCOUNT estimates 10000 distinct visitors as ${pf}; the union with mobile as ${pfUnion}`] =
    Math.abs(pf - 10000) / 10000 < 0.02 && Math.abs(pfUnion - (10000 + 2000 - 1429)) / 10571 < 0.03;
let wrongType = null;
try {
    await cmd("LPUSH", "leaderboard", "x");
} catch (e) {
    wrongType = String(e.message || e);
}
results["type errors are WRONGTYPE replies"] = /WRONGTYPE/.test(wrongType || "");

// Pipelining: 2,000 commands in one write.
const t0 = performance.now();
const replies = await r.pipeline(Array.from({ length: 2000 }, (_, i) => ["SET", `bulk:${i}`, `value-${i}`]));
const pipeMs = performance.now() - t0;
results[`2000 pipelined SETs answered in order (${pipeMs.toFixed(0)} ms)`] =
    replies.length === 2000 && replies.every((x) => x === "OK");

// Transactions with WATCH from a second connection.
const r2 = new Redis({ host: "127.0.0.1", port: server.srv.port });
await cmd("WATCH", "visits");
await cmd("MULTI");
await cmd("INCR", "visits");
await cmd("INCR", "visits");
await r2.command("SET", "visits", "100");
const aborted = await cmd("EXEC");
await cmd("MULTI");
await cmd("INCR", "visits");
await cmd("GET", "visits");
const committed = await cmd("EXEC");
results["WATCH aborts a transaction whose key changed; MULTI/EXEC runs atomically"] =
    aborted === null && JSON.stringify(committed) === '[101,"101"]';

// Pub/sub.
const messages = [];
const sub = new Redis({ host: "127.0.0.1", port: server.srv.port });
sub.on("message", (m) => messages.push(m));
sub.on("push", (m) => messages.push(m));
await sub.command("SUBSCRIBE", "orders");
const receivers = await cmd("PUBLISH", "orders", "order-1042 created");
await sleep(50);
results["PUBLISH reaches the subscriber"] = receivers === 1 && JSON.stringify(messages).includes("order-1042 created");

// Blocking pops: waiters are served oldest first, a blocked client's later commands wait behind it, a timeout answers
// nil, and inside MULTI the pop never blocks.
{
    const port = server.srv.port,
        b1 = new Redis({ host: "127.0.0.1", port }),
        b2 = new Redis({ host: "127.0.0.1", port }),
        order = [];
    const p1 = b1.command("BLPOP", "jobs", "other", 5);
    await sleep(20);
    const p2 = b2.command("BLPOP", "jobs", 5);
    await sleep(20);
    const pushed = await cmd("RPUSH", "jobs", "a", "b", "c");
    const [g1, g2] = await Promise.all([p1, p2]);
    results["BLPOP serves blocked clients in arrival order after the push replies"] =
        pushed === 3 &&
        JSON.stringify([g1, g2]) === '[["jobs","a"],["jobs","b"]]' &&
        JSON.stringify(await cmd("LRANGE", "jobs", 0, -1)) === '["c"]';
    const blocked = b2.command("BRPOP", "q2", 5).then((v) => order.push(["pop", v]));
    const ping = b2.command("ECHO-LESS-PING").then(
        () => order.push(["after", "reply"]),
        (e) => order.push(["after", e.code]),
    );
    await sleep(30);
    const parkedFirst = order.length === 0;
    await cmd("RPUSH", "q2", "x", "y");
    await Promise.all([blocked, ping]);
    results["a blocked client's pipelined command waits for the pop"] =
        parkedFirst && JSON.stringify(order) === '[["pop",["q2","y"]],["after","ERR"]]';
    const t = performance.now(),
        none = await b1.command("BLPOP", "nothing-here", 0.1),
        waited = performance.now() - t;
    results[`BLPOP times out with nil (${waited.toFixed(0)} ms for 100)`] =
        none === null && waited >= 90 && waited < 1000;
    await cmd("MULTI");
    await cmd("BLPOP", "nothing-here", 5);
    results["BLPOP inside MULTI does not block"] = JSON.stringify(await cmd("EXEC")) === "[null]";
    // the pops were logged after the pushes that fed them: a store replaying this AOF has the same lists
    store.drain();
    const copy = work.join("blpop.aof");
    writeFile(copy, readFile(aofPath));
    const replayed = new Store({ aofPath: copy });
    results["served blocking pops replay from the AOF"] =
        JSON.stringify(replayed.data.get("jobs")?.value.toArray()) === '["c"]' &&
        JSON.stringify(replayed.data.get("q2")?.value.toArray()) === '["x"]';
    replayed.close();
    b1.close();
    b2.close();
}

// Expiry: lazy and active.
for (let i = 0; i < 300; i++) await cmd("SET", `tmp:${i}`, "x", "PX", 50);
await sleep(250);
results[`active expiry removed ${store.stats.expired} keys without them being read`] =
    store.stats.expired >= 250 && (await cmd("GET", "session:1")) === null;

// Eviction under maxmemory.
for (let i = 0; i < 400; i++) {
    await cmd("SET", `blob:${i}`, "x".repeat(1024));
    if (i % 5 === 0) await cmd("GET", "greeting");
} // a hot key stays in use
results[`allkeys-lru evicted ${store.stats.evicted} keys and kept the hot one`] =
    store.stats.evicted > 0 && store.used <= store.maxmemory && (await cmd("GET", "greeting")) !== null;

// Persistence: rewrite the AOF, restart, compare.
await cmd("PFADD", "uniques", ...Array.from({ length: 3000 }, (_, i) => `u${i}`));
const uniquesBefore = await cmd("PFCOUNT", "uniques");
const firstRun = { ...store.stats };
const beforeKeys = (await cmd("KEYS", "*")).sort(),
    beforeLb = await cmd("ZRANGE", "leaderboard", 0, -1, "WITHSCORES");
const aofBefore = stat(aofPath).size;
await cmd("BGREWRITEAOF");
const aofAfter = stat(aofPath).size;
r.close();
r2.close();
sub.close();
server.stop();
store = new Store({ aofPath, maxmemory: 256 * 1024 });
server = startServer(store);
const r3 = new Redis({ host: "127.0.0.1", port: server.srv.port });
const afterKeys = (await r3.command("KEYS", "*")).sort();
results[`a HyperLogLog survives the AOF rewrite and restart (${uniquesBefore})`] =
    (await r3.command("PFCOUNT", "uniques")) === uniquesBefore && Math.abs(uniquesBefore - 3000) < 60;
results[`AOF rewrite ${aofBefore} -> ${aofAfter} bytes; a restarted server has the same ${afterKeys.length} keys`] =
    aofAfter < aofBefore &&
    JSON.stringify(afterKeys) === JSON.stringify(beforeKeys) &&
    JSON.stringify(await r3.command("ZRANGE", "leaderboard", 0, -1, "WITHSCORES")) === JSON.stringify(beforeLb);
const info = await r3.command("INFO");
r3.close();
server.stop();

// =============================================================================
// Model-based differential: a random command stream against a plain model, then two restarts from the AOF
// =============================================================================
// The model keeps strings, arrays, objects and score maps and recomputes every reply from Redis's documented
// semantics (negative LRANGE indices, empty containers vanish, WRONGTYPE, byte-length APPEND, ZSET order by score
// then member) without a Deque, ZSet or the server's code.
const WRONG = { err: "WRONGTYPE" },
    NOTINT = { err: "ERR" };
function modelExec(m, [c, k, ...x]) {
    const v = m.get(k),
        is = (t) => v === undefined || v.t === t;
    const zsorted = (z) => [...z].sort((p, q) => p[1] - q[1] || (p[0] < q[0] ? -1 : p[0] > q[0] ? 1 : 0));
    const isInt = (s) => /^-?\d+$/.test(s);
    switch (c) {
        case "SET":
            m.set(k, { t: "string", v: x[0] });
            return "OK";
        case "GET":
            return !is("string") ? WRONG : (v?.v ?? null);
        case "INCRBY":
            if (!is("string")) return WRONG;
            if (v && !isInt(v.v)) return NOTINT;
            m.set(k, { t: "string", v: String(Number(v?.v ?? 0) + Number(x[0])) });
            return Number(m.get(k).v);
        case "APPEND":
            if (!is("string")) return WRONG;
            m.set(k, { t: "string", v: (v?.v ?? "") + x[0] });
            return enc.encode(m.get(k).v).length;
        case "DEL":
            return [k, ...x].filter((key) => m.delete(key)).length;
        case "EXISTS":
            return [k, ...x].filter((key) => m.has(key)).length;
        case "TYPE":
            return v ? v.t : "none";
        case "LPUSH":
        case "RPUSH": {
            if (!is("list")) return WRONG;
            const l = v?.v ?? [];
            for (const e of x) c === "LPUSH" ? l.unshift(e) : l.push(e);
            m.set(k, { t: "list", v: l });
            return l.length;
        }
        case "LPOP":
        case "RPOP": {
            if (!is("list")) return WRONG;
            if (!v) return null;
            const e = c === "LPOP" ? v.v.shift() : v.v.pop();
            if (!v.v.length) m.delete(k);
            return e;
        }
        case "LRANGE": {
            if (!is("list")) return WRONG;
            const l = v?.v ?? [],
                n = l.length;
            let a = Number(x[0]),
                b = Number(x[1]);
            if (a < 0) a = Math.max(0, n + a);
            if (b < 0) b = n + b;
            return a > b || a >= n ? [] : l.slice(a, Math.min(b, n - 1) + 1);
        }
        case "HSET": {
            if (!is("hash")) return WRONG;
            const h = v?.v ?? {};
            let added = 0;
            for (let i = 0; i + 1 < x.length; i += 2) {
                if (!Object.hasOwn(h, x[i])) added++;
                h[x[i]] = x[i + 1];
            }
            m.set(k, { t: "hash", v: h });
            return added;
        }
        case "HGET":
            return !is("hash") ? WRONG : v && Object.hasOwn(v.v, x[0]) ? v.v[x[0]] : null;
        case "HDEL": {
            if (!is("hash")) return WRONG;
            if (!v) return 0;
            let n = 0;
            for (const f of x) if (Object.hasOwn(v.v, f)) (delete v.v[f], n++);
            if (!Object.keys(v.v).length) m.delete(k);
            return n;
        }
        case "HINCRBY": {
            if (!is("hash")) return WRONG;
            const h = v?.v ?? {};
            if (Object.hasOwn(h, x[0]) && !isInt(h[x[0]])) return NOTINT;
            h[x[0]] = String(Number(h[x[0]] ?? 0) + Number(x[1]));
            m.set(k, { t: "hash", v: h });
            return Number(h[x[0]]);
        }
        case "ZADD":
        case "ZINCRBY": {
            if (!is("zset")) return WRONG;
            const z = v?.v ?? new Map();
            const sc = Number(x[0]),
                had = z.has(x[1]);
            z.set(x[1], c === "ZINCRBY" ? (z.get(x[1]) ?? 0) + sc : sc);
            m.set(k, { t: "zset", v: z });
            return c === "ZINCRBY" ? String(z.get(x[1])) : had ? 0 : 1;
        }
        case "ZREM": {
            if (!is("zset")) return WRONG;
            if (!v) return 0;
            const n = x.filter((e) => v.v.delete(e)).length;
            if (!v.v.size) m.delete(k);
            return n;
        }
        case "ZSCORE":
            return !is("zset") ? WRONG : v?.v.has(x[0]) ? String(v.v.get(x[0])) : null;
        case "ZRANK": {
            if (!is("zset")) return WRONG;
            const i = v ? zsorted(v.v).findIndex(([e]) => e === x[0]) : -1;
            return i < 0 ? null : i;
        }
        case "ZRANGE": {
            if (!is("zset")) return WRONG;
            const all = v ? zsorted(v.v) : [],
                n = all.length;
            let a = Number(x[0]),
                b = Number(x[1]);
            if (a < 0) a = Math.max(0, n + a);
            if (b < 0) b = n + b;
            const sel = a > b || a >= n ? [] : all.slice(a, Math.min(b, n - 1) + 1);
            return sel.flatMap(([e, sc]) => [e, String(sc)]);
        }
        case "ZRANGEBYSCORE": {
            if (!is("zset")) return WRONG;
            const lo = Number(x[0].slice(1)),
                hi = Number(x[1]); // "(lo" exclusive, hi inclusive
            return (v ? zsorted(v.v) : []).filter(([, sc]) => sc > lo && sc <= hi).map(([e]) => e);
        }
    }
    throw new Error(`model has no ${c}`);
}
const norm = (r) => (r && typeof r === "object" && r.redis ? { err: r.code } : r);
function* commandStream(rnd, n) {
    const key = (t) => `${t}${rnd.nextBounded(4)}`,
        anyKey = () => key("sxlhz"[rnd.nextBounded(5)] === "x" ? "s" : "slhz"[rnd.nextBounded(4)]),
        val = () =>
            ["", "v", "héllo", "日本語", "🚀x", "12", "-7", "3.5", "a b"][rnd.nextBounded(9)] + rnd.nextBounded(3),
        num = () => String(rnd.nextBounded(21) - 10),
        member = () => `m${rnd.nextBounded(6)}`,
        score = () => String((rnd.nextBounded(9) - 4) / 2);
    for (let i = 0; i < n; i++) {
        // one in ten commands goes to a key of a random type, exercising WRONGTYPE
        const k = (t) => (rnd.nextBounded(10) === 0 ? anyKey() : key(t));
        const op = rnd.nextBounded(25);
        yield [
            () => ["SET", k("s"), val()],
            () => ["SET", k("s"), num()],
            () => ["GET", k("s")],
            () => ["INCRBY", k("s"), num()],
            () => ["APPEND", k("s"), val()],
            () => ["DEL", anyKey(), anyKey()],
            () => ["EXISTS", anyKey(), anyKey()],
            () => ["TYPE", anyKey()],
            () => ["LPUSH", k("l"), val(), val()],
            () => ["RPUSH", k("l"), val()],
            () => ["LPOP", k("l")],
            () => ["RPOP", k("l")],
            () => ["LRANGE", k("l"), num(), num()],
            () => ["HSET", k("h"), member(), val(), member(), num()],
            () => ["HGET", k("h"), member()],
            () => ["HDEL", k("h"), member(), member()],
            () => ["HINCRBY", k("h"), member(), num()],
            () => ["ZADD", k("z"), score(), member()],
            () => ["ZINCRBY", k("z"), score(), member()],
            () => ["ZREM", k("z"), member()],
            () => ["ZSCORE", k("z"), member()],
            () => ["ZRANK", k("z"), member()],
            () => ["ZRANGE", k("z"), num(), num(), "WITHSCORES"],
            () => ["ZRANGEBYSCORE", k("z"), `(${score()}`, score()],
            () => ["HSET", k("h"), member(), val()],
        ][op]();
    }
}
{
    const diffAof = work.join("diff.aof"),
        model = new Map(),
        rnd = new Random(42);
    let st = new Store({ aofPath: diffAof }),
        sv = startServer(st),
        c = new Redis({ host: "127.0.0.1", port: sv.srv.port });
    let agree = 0,
        total = 0,
        wrongType = 0,
        firstMismatch = null;

    for (const batch of commandStream(rnd, 4000).chunks(250)) {
        const got = await c.pipeline(batch);
        batch.forEach((command, i) => {
            const want = JSON.stringify(modelExec(model, command)),
                have = JSON.stringify(norm(got[i]));
            total++;
            if (want === '{"err":"WRONGTYPE"}') wrongType++;
            if (want === have) agree++;
            else firstMismatch ??= `${JSON.stringify(command)}: server ${have}, model ${want}`;
        });
    }
    results[`4000 random commands answer exactly as the model (${wrongType} WRONGTYPE)`] =
        agree === total && wrongType > 50 && firstMismatch === null;
    if (firstMismatch) print(`  first mismatch ${firstMismatch}`);
    const modelDump = () => {
        const out = {};
        for (const k of [...model.keys()].sort()) {
            const { t, v } = model.get(k);
            out[k] =
                t === "string"
                    ? v
                    : t === "list"
                      ? v
                      : t === "hash"
                        ? Object.entries(v).sort()
                        : [...v]
                              .sort((p, q) => p[1] - q[1] || (p[0] < q[0] ? -1 : 1))
                              .flatMap(([e, sc]) => [e, String(sc)]);
        }
        return JSON.stringify(out);
    };
    const serverDump = async () => {
        const out = {};
        for (const k of (await c.command("KEYS", "*")).sort()) {
            const t = await c.command("TYPE", k);
            if (t === "string") out[k] = await c.command("GET", k);
            else if (t === "list") out[k] = await c.command("LRANGE", k, 0, -1);
            else if (t === "zset") out[k] = await c.command("ZRANGE", k, 0, -1, "WITHSCORES");
            else {
                const h = await c.command("HGETALL", k);
                out[k] = (Array.isArray(h) ? h.values().chunks(2).toArray() : Object.entries(h)).sort();
            }
        }
        return JSON.stringify(out);
    };
    const live = await serverDump(),
        want = modelDump();
    const restart = async () => {
        c.close();
        sv.stop();
        st = new Store({ aofPath: diffAof });
        sv = startServer(st);
        c = new Redis({ host: "127.0.0.1", port: sv.srv.port });
    };
    await restart();
    const fromLog = await serverDump();
    await c.command("BGREWRITEAOF");
    await restart();
    const fromRewrite = await serverDump();
    results[`the final keyspace equals the model live, after an AOF replay, and after a rewrite (${model.size} keys)`] =
        live === want && fromLog === want && fromRewrite === want && model.size > 10;
    c.close();
    sv.stop();
}

// The parser sees TCP's arbitrary splits: feed the same RESP bytes whole and in random 1..40-byte pieces (cutting
// inside headers, CRLFs and multi-byte UTF-8) and require the same commands.
{
    const rnd = new Random(7),
        sent = [...commandStream(rnd, 600)];
    const wire = enc.encode(
        sent.map((a) => `*${a.length}\r\n${a.map((x) => `$${enc.encode(x).length}\r\n${x}\r\n`).join("")}`).join("") +
            "PING\r\nECHO  spaced   inline\r\n",
    );
    const parsed = [];
    const feed = parser((args) => parsed.push(args));
    for (let at = 0; at < wire.length;) {
        const n = 1 + rnd.nextBounded(40);
        feed(wire.subarray(at, at + n));
        at += n;
    }
    const whole = [];
    parser((args) => whole.push(args))(wire);
    results[
        `${wire.length} bytes of RESP fed in random 1-40-byte pieces parse to the same ${sent.length + 2} commands`
    ] =
        JSON.stringify(parsed) === JSON.stringify([...sent, ["PING"], ["ECHO", "spaced", "inline"]]) &&
        JSON.stringify(whole) === JSON.stringify(parsed);
}

print("redis-compatible-store");
print(
    table([
        ["measure", "value"],
        ["pipelined SET x2000", `${pipeMs.toFixed(1)} ms`],
        ["expired / evicted keys", `${firstRun.expired} / ${firstRun.evicted}`],
        ["keys after restart", String(afterKeys.length)],
        ["AOF before/after rewrite", `${aofBefore} / ${aofAfter} bytes`],
        [
            "info",
            String(info)
                .split("\r\n")
                .filter((l) => l.includes(":"))
                .slice(0, 3)
                .join(" "),
        ],
    ]),
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
