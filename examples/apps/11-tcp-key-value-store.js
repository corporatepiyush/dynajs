// 11 · TCP key-value store — a line protocol server with TTLs, an LRU bound and a pipelining client.
//
// WHAT IT SHOWS
//   - dyna:net TCPServer: connect/data/close handlers and per-connection state
//   - framing: TCP delivers bytes, not messages, so the server buffers until a newline
//   - dyna:structures LRU: the store can never outgrow its capacity
//
// PROTOCOL (one command per line, one reply per line)
//   SET key value [ttlMs]   -> OK
//   GET key                 -> VALUE <value> | NIL
//   DEL key                 -> 1 | 0
//   STATS                   -> hits=.. misses=.. size=..
//
// RUN      dynajs examples/apps/11-tcp-key-value-store.js
// DEPLOY   PORT=7000 CAPACITY=100000 dynajs examples/apps/11-tcp-key-value-store.js

import { TCPServer } from "dyna:net";
import { LRU } from "dyna:structures";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const MAX_LINE = 64 * 1024;                      // a peer cannot make us buffer without bound
const store = new LRU(Number(getEnv("CAPACITY") ?? 1000));

function execute(line) {
    const parts = line.split(" ");
    const cmd = parts[0].toUpperCase();
    if (cmd === "SET" && parts.length >= 3) {
        const ttl = Number(parts[3] ?? 0);
        if (ttl > 0) store.setWithTTL(parts[1], parts[2], ttl); else store.set(parts[1], parts[2]);
        return "OK";
    }
    if (cmd === "GET" && parts.length === 2) {
        const value = store.get(parts[1]);
        return value === undefined ? "NIL" : "VALUE " + value;
    }
    if (cmd === "DEL" && parts.length === 2) return store.delete(parts[1]) ? "1" : "0";
    if (cmd === "STATS") { const s = store.stats; return `hits=${s.hits} misses=${s.misses} size=${s.size}`; }
    return "ERR unknown command";
}

// Per-connection receive buffers. A WeakMap would also work; a Map with
// explicit cleanup in close() makes the lifetime obvious.
const pending = new Map();
const decoder = new TextDecoder();

const server = new TCPServer({ port: PORT, idleTimeoutMs: 60000, maxConnections: 1024 });
server.start({
    connect: (conn) => pending.set(conn, ""),
    data: (conn, bytes) => {
        let buffer = pending.get(conn) + decoder.decode(bytes);
        let newline;
        let replies = "";
        // One read may hold several commands (pipelining) or half of one.
        while ((newline = buffer.indexOf("\n")) >= 0) {
            const line = buffer.slice(0, newline).replace(/\r$/, "");
            buffer = buffer.slice(newline + 1);
            if (line) replies += execute(line) + "\n";
        }
        if (buffer.length > MAX_LINE) return conn.close();   // no newline in 64 KiB: not our protocol
        pending.set(conn, buffer);
        if (replies) conn.write(replies);                    // one write for the whole batch
    },
    close: (conn) => pending.delete(conn),
});
console.log("key-value store on port", server.port);

// ---- self-test: a client that pipelines commands ---------------------------
if (!PORT) {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };

    // send(lines) resolves with one reply per command, in order.
    function connect(port) {
        let buffer = "";
        const waiting = [];
        let handle;                                  // held here so the connection is not collected
        const ready = new Promise((resolve, reject) => {
            handle = TCPServer.connect({ host: "127.0.0.1", port }, {
                connect: (c, err) => err ? reject(new Error(err)) : resolve(c),
                data: (_c, bytes) => {
                    buffer += decoder.decode(bytes);
                    while (waiting.length) {
                        const lines = buffer.split("\n");
                        if (lines.length - 1 < waiting[0].count) break;
                        const job = waiting.shift();
                        buffer = lines.slice(job.count).join("\n");
                        job.resolve(lines.slice(0, job.count));
                    }
                },
            });
        });
        return {
            send: async (commands) => {
                const conn = await ready;
                return new Promise((resolve) => {
                    waiting.push({ count: commands.length, resolve });
                    conn.write(commands.join("\n") + "\n");
                });
            },
            close: () => handle.close(),
        };
    }

    const client = connect(server.port);
    const replies = await client.send(["SET user:1 ada", "GET user:1", "GET user:2", "DEL user:1", "GET user:1"]);
    check(replies.join("|") === "OK|VALUE ada|NIL|1|NIL", "pipelined replies arrive in order: " + replies.join("|"));

    await client.send(["SET session:9 token 30"]);            // expires after 30 ms
    check((await client.send(["GET session:9"]))[0] === "VALUE token", "a fresh TTL entry is readable");
    await sleep(60);
    check((await client.send(["GET session:9"]))[0] === "NIL", "the entry expires");
    check((await client.send(["FLY away"]))[0].startsWith("ERR"), "unknown commands get an error line");
    console.log("self-test passed:", (await client.send(["STATS"]))[0]);
    client.close();
    server.close();
}
