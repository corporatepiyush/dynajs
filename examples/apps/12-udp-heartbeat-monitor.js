// 12 · UDP heartbeat monitor — agents send signed heartbeats; the monitor flags nodes that go quiet.
//
// WHAT IT SHOWS
//   - dyna:net UDPSocket: datagrams in both directions with the sender's address
//   - authenticating datagrams with an HMAC, since UDP source addresses are trivially spoofed
//   - dyna:bytes fixed-width writes for a compact binary wire format
//
// WIRE FORMAT (big-endian):  u32 sequence | f64 load | u8 name length | name | 32-byte HMAC-SHA256
//
// RUN      dynajs examples/apps/12-udp-heartbeat-monitor.js
// DEPLOY   PORT=9999 HEARTBEAT_KEY=... dynajs examples/apps/12-udp-heartbeat-monitor.js

import { UDPSocket } from "dyna:net";
import { HMAC, TimingSafeEqual } from "dyna:crypto";
import { getEnv } from "dyna:sys";

const PORT = Number(getEnv("PORT") ?? 0);
const KEY = getEnv("HEARTBEAT_KEY") ?? "dev-only-heartbeat-key";
const TIMEOUT_MS = PORT ? 15000 : 120;           // silence longer than this means "down"
const MAC_LEN = 32;

// ---- encoding --------------------------------------------------------------
function encode(seq, load, name) {
    const nameBytes = new TextEncoder().encode(name);
    const body = new Uint8Array(4 + 8 + 1 + nameBytes.length);
    const view = new DataView(body.buffer);
    view.setUint32(0, seq);
    view.setFloat64(4, load);
    view.setUint8(12, nameBytes.length);
    body.set(nameBytes, 13);
    const packet = new Uint8Array(body.length + MAC_LEN);
    packet.set(body);
    packet.set(HMAC("sha256", KEY, body), body.length);
    return packet;
}

// Returns the decoded heartbeat, or null for anything malformed or unsigned.
// Every length is checked before it is used: datagrams are untrusted input.
function decode(packet) {
    if (packet.length < 13 + MAC_LEN) return null;
    const body = packet.subarray(0, packet.length - MAC_LEN);
    const mac = packet.subarray(packet.length - MAC_LEN);
    if (!TimingSafeEqual(HMAC("sha256", KEY, body), mac)) return null;
    const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
    const nameLen = view.getUint8(12);
    if (13 + nameLen !== body.length) return null;
    return { seq: view.getUint32(0), load: view.getFloat64(4),
             name: new TextDecoder().decode(body.subarray(13, 13 + nameLen)) };
}

// ---- monitor ---------------------------------------------------------------
const nodes = new Map();                         // name -> { seq, load, lastSeen, from }
let rejected = 0;

const monitor = new UDPSocket({ port: PORT, host: "127.0.0.1" });
monitor.start({
    message: (data, from) => {
        const beat = decode(data);
        if (!beat) { rejected++; return; }
        const known = nodes.get(beat.name);
        // Sequence numbers only move forward: an old packet replayed by an
        // attacker (or reordered by the network) cannot refresh a dead node.
        if (known && beat.seq <= known.seq) { rejected++; return; }
        nodes.set(beat.name, { seq: beat.seq, load: beat.load, lastSeen: Date.now(), from: from.address });
    },
});

const report = () => [...nodes].map(([name, n]) =>
    ({ name, load: n.load, state: Date.now() - n.lastSeen > TIMEOUT_MS ? "DOWN" : "up" }));

console.log("heartbeat monitor on UDP port", monitor.port);
if (PORT) setInterval(() => console.log(JSON.stringify(report())), 5000);

// ---- self-test: two agents, one of which stops ------------------------------
if (!PORT) {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const agent = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const beat = (name, seq, load) => agent.send(encode(seq, load, name), "127.0.0.1", monitor.port);

    beat("web-1", 1, 0.42);
    beat("db-1", 1, 1.75);
    await sleep(40);
    check(report().length === 2 && report().every((n) => n.state === "up"), "both nodes report in");

    // Garbage, a forged packet and a replay must all be dropped.
    agent.send(new Uint8Array([1, 2, 3]), "127.0.0.1", monitor.port);
    const forged = encode(99, 0, "db-1"); forged[forged.length - 1] ^= 0xff;
    agent.send(forged, "127.0.0.1", monitor.port);
    beat("web-1", 1, 0.0);                                   // replay of sequence 1
    await sleep(40);
    check(rejected === 3, "malformed, forged and replayed packets are rejected: " + rejected);
    check(nodes.get("web-1").load === 0.42, "the replay did not overwrite the real reading");

    // web-1 keeps beating; db-1 goes silent.
    for (let seq = 2; seq <= 5; seq++) { beat("web-1", seq, 0.5); await sleep(40); }
    const states = Object.fromEntries(report().map((n) => [n.name, n.state]));
    check(states["web-1"] === "up" && states["db-1"] === "DOWN", "the silent node is flagged: " + JSON.stringify(states));
    console.log("self-test passed:", JSON.stringify(states), "| rejected packets:", rejected);
    agent.close();
    monitor.close();
}
