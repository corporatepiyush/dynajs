// 48 · Binary RPC over TCP — length-prefixed MessagePack frames, request ids, pipelining and hard size limits.
//
// WHAT IT SHOWS
//   - dyna:serialize MsgPackEncode/MsgPackDecode: compact binary messages, with a decoder hardened for hostile input
//   - dyna:encoding varints as the length prefix: one byte for small frames, no fixed 4-byte tax
//   - reassembling frames from a byte stream that splits and merges them arbitrarily
//   - matching replies to requests by id, so calls can be pipelined and answered out of order
//   - a frame-size cap enforced BEFORE buffering, so a peer cannot make the server allocate at will
//
// FRAME    uvarint length | MessagePack { id, method, params }      reply: { id, result } or { id, error }
//
// RUN      dynajs examples/apps/48-binary-rpc-protocol.js

import { TCPServer } from "dyna:net";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { PutUvarint, Uvarint } from "dyna:encoding";

const MAX_FRAME = 64 * 1024;

// ---- framing, shared by both ends -------------------------------------------
function frame(message) {
    const body = MsgPackEncode(message);
    const prefix = PutUvarint(body.length);
    const out = new Uint8Array(prefix.length + body.length);
    out.set(prefix); out.set(body, prefix.length);
    return out;
}

// A Deframer accumulates bytes and yields whole messages. It throws on a frame
// that announces more than MAX_FRAME, which the caller answers by hanging up.
class Deframer {
    constructor() { this.buffer = new Uint8Array(0); }
    push(bytes) {
        const joined = new Uint8Array(this.buffer.length + bytes.length);
        joined.set(this.buffer); joined.set(bytes, this.buffer.length);
        this.buffer = joined;
        const messages = [];
        for (;;) {
            // Uvarint returns [value, bytesRead]; bytesRead 0 means "need more bytes".
            const [length, used] = Uvarint(this.buffer);
            if (used === 0) {
                if (this.buffer.length >= 10) throw new Error("malformed length prefix");
                break;
            }
            if (length > MAX_FRAME) throw new Error(`frame of ${length} bytes exceeds the ${MAX_FRAME} limit`);
            if (this.buffer.length < used + Number(length)) break;          // body still arriving
            messages.push(MsgPackDecode(this.buffer.subarray(used, used + Number(length))));
            this.buffer = this.buffer.slice(used + Number(length));
        }
        return messages;
    }
}

// ---- server ----------------------------------------------------------------
const methods = {
    add: ([a, b]) => a + b,
    upper: ([text]) => String(text).toUpperCase(),
    checksum: ([bytes]) => bytes.reduce((sum, b) => (sum + b) & 0xffff, 0),   // binary params arrive as Uint8Array
    slow: async ([ms]) => { await sleep(ms); return "done after " + ms; },
};

const deframers = new Map();
const server = new TCPServer({ port: 0, idleTimeoutMs: 30000 });
server.start({
    connect: (conn) => deframers.set(conn, new Deframer()),
    data: (conn, bytes) => {
        let requests;
        try { requests = deframers.get(conn).push(bytes); }
        catch (e) { conn.close(); return; }                // protocol violation: no reply, just hang up
        for (const req of requests) {
            const fn = Object.hasOwn(methods, req?.method) ? methods[req.method] : null;
            // Each call settles on its own; a slow one does not hold up the others.
            Promise.resolve()
                .then(() => { if (!fn) throw new Error("unknown method: " + req?.method); return fn(req.params ?? []); })
                .then((result) => conn.write(frame({ id: req.id, result })),
                      (err) => conn.write(frame({ id: req.id, error: err.message })));
        }
    },
    close: (conn) => deframers.delete(conn),
});
console.log("binary RPC on port", server.port);

// ---- client ----------------------------------------------------------------
function connect(port) {
    const pending = new Map();                 // id -> { resolve, reject }
    const deframer = new Deframer();
    let nextId = 1, conn, closed = false;
    // Keep the handle in this closure for as long as the client lives: a
    // connection nobody references is collected, and collection closes it.
    let handle;
    const ready = new Promise((resolve, reject) => {
        handle = TCPServer.connect({ host: "127.0.0.1", port }, {
            connect: (c, err) => { if (err) reject(new Error(err)); else { conn = c; resolve(); } },
            data: (_c, bytes) => {
                for (const reply of deframer.push(bytes)) {
                    const waiter = pending.get(reply.id);
                    if (!waiter) continue;                 // a reply nobody asked for is ignored
                    pending.delete(reply.id);
                    if ("error" in reply) waiter.reject(new Error(reply.error)); else waiter.resolve(reply.result);
                }
            },
            close: () => {
                closed = true;
                for (const waiter of pending.values()) waiter.reject(new Error("connection closed"));
                pending.clear();
            },
        });
    });
    return {
        call: async (method, ...params) => {
            await ready;
            if (closed) throw new Error("connection closed");
            const id = nextId++;
            return new Promise((resolve, reject) => {
                pending.set(id, { resolve, reject });
                conn.write(frame({ id, method, params }));
            });
        },
        raw: async (bytes) => { await ready; conn.write(bytes); },
        get closed() { return closed; },
        close: () => handle.close(),
    };
}

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const failure = (p) => p.then(() => "", (e) => e.message);

const client = connect(server.port);
check(await client.call("add", 2, 40) === 42, "a call round-trips");
check(await client.call("upper", "héllo") === "HÉLLO", "strings survive as UTF-8");
check(await client.call("checksum", new Uint8Array([250, 10, 1])) === 261, "binary parameters arrive as bytes");
check((await failure(client.call("nope"))).includes("unknown method"), "errors come back as errors");
check((await failure(client.call("constructor"))).includes("unknown method"), "inherited names are not callable methods");

// Pipelining: the slow call is sent first but answered last.
const order = [];
await Promise.all([
    client.call("slow", 60).then((r) => order.push("slow")),
    client.call("add", 1, 1).then((r) => order.push("fast")),
]);
check(order.join() === "fast,slow", "replies are matched by id, not by arrival order");

// Framing survives any chunking: 50 calls written in one burst.
const burst = await Promise.all(Array.from({ length: 50 }, (_, i) => client.call("add", i, i)));
check(burst.every((v, i) => v === 2 * i), "fifty pipelined calls all resolve correctly");

// The Deframer itself, fed one byte at a time.
const d = new Deframer();
const two = new Uint8Array([...frame({ id: 1, method: "a" }), ...frame({ id: 2, method: "b" })]);
const got = [];
for (const byte of two) got.push(...d.push(new Uint8Array([byte])));
check(got.length === 2 && got[1].method === "b", "frames reassemble from single-byte reads");

// A peer announcing a huge frame is disconnected without the server buffering it.
const hostile = connect(server.port);
await hostile.raw(PutUvarint(500 * 1024 * 1024));
for (let i = 0; i < 100 && !hostile.closed; i++) await sleep(5);
check(hostile.closed, "an oversized frame announcement closes the connection");
check(await client.call("add", 1, 2) === 3, "other connections are unaffected");

console.log("self-test passed: framing, pipelining, binary params, size limit");
// Every client handle must be closed, including the one the server hung up
// on: an open handle keeps the event loop, and so the process, alive.
hostile.close();
client.close();
server.close();
