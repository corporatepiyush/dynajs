// covers: dyna:webrtc.RTCDataChannel -- two dynajs processes connected over real loopback UDP: the child
// (webrtc_peer_child.js) answers or offers, signaling travels as JSON over a UDPSocket, and the child echoes every
// message. Proves ICE, DTLS (both roles: the offerer is the DTLS server), SCTP and DCEP across a process boundary:
// ordered strings, empty and binary messages, a 1 MiB message, and close initiated by either side.
// slots: 2
import { RTCPeerConnection, RTCDataChannel } from "dyna:webrtc";
import { UDPSocket } from "dyna:net";
import { getEnv } from "dyna:sys";
import { suite } from "../lib/kit.js";
import { spawnHelper } from "../lib/helper.js";
import { LO, waitEvent, waitState } from "./rtc_util.js";

const t = suite("webrtc_two_process");
const BIN = getEnv("DYNAJS_CHILD_BIN") || "./dynajs";

function peer(role) {
    const sig = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const pc = new RTCPeerConnection(LO);
    let childPort = 0;
    const queue = [];
    const send = (o) => {
        if (!childPort) { queue.push(o); return; }
        sig.send(JSON.stringify(o), "127.0.0.1", childPort);
    };
    const ready = new Promise((res) => {
        sig.start({
            message: async (d, from) => {
                const m = JSON.parse(new TextDecoder().decode(d));
                if (m.t === "hello") {
                    childPort = from.port;
                    for (const q of queue.splice(0)) send(q);
                    res();
                } else if (m.t === "desc") {
                    await pc.setRemoteDescription(m.d);
                    if (m.d.type === "offer") {
                        await pc.setLocalDescription(await pc.createAnswer());
                        send({ t: "desc", d: pc.localDescription.toJSON() });
                    }
                } else if (m.t === "cand") {
                    await pc.addIceCandidate(m.c).catch(() => {});
                }
            },
        });
    });
    pc.onicecandidate = (e) => send({ t: "cand", c: e.candidate ? e.candidate.toJSON() : null });
    const child = spawnHelper(BIN + " tests/webrtc/webrtc_peer_child.js " + sig.port + " " + role);
    return { pc, sig, child, ready, send };
}

function roundTrip(ch, msgs, ms = 20000) {
    return new Promise((res, rej) => {
        const got = [];
        const tm = setTimeout(() => rej(new Error("echoed " + got.length + "/" + msgs.length)), ms);
        ch.onmessage = (e) => { got.push(e.data); if (got.length === msgs.length) { clearTimeout(tm); res(got); } };
        for (const m of msgs) ch.send(m);
    });
}

async function exercise(a, ch) {
    a.ok(ch instanceof RTCDataChannel);
    ch.binaryType = "arraybuffer";
    const big = new Uint8Array(1 << 20);
    for (let i = 0; i < big.length; i++) big[i] = (i * 2654435761) >>> 24;
    const msgs = [];
    for (let i = 0; i < 100; i++) msgs.push("s" + i);
    msgs.push("", new Uint8Array(0), new Uint8Array([1, 2, 3]), big, "end");
    const got = await roundTrip(ch, msgs, 30000);
    for (let i = 0; i < 100; i++) a.eq(got[i], "s" + i);
    a.eq(got[100], "");
    a.ok(got[101] instanceof ArrayBuffer && got[101].byteLength === 0);
    a.deep(Array.from(new Uint8Array(got[102])), [1, 2, 3]);
    const rb = new Uint8Array(got[103]);
    a.eq(rb.length, big.length);
    let same = true;
    for (let i = 0; i < rb.length; i += 1) if (rb[i] !== big[i]) { same = false; break; }
    a.ok(same, "1 MiB echo intact");
    a.eq(got[104], "end");
}

t.test("parent offers (DTLS server), child answers; child closes on request", async (a) => {
    const p = peer("answer");
    try {
        await p.ready;
        const ch = p.pc.createDataChannel("two-proc", { protocol: "echo" });
        await p.pc.setLocalDescription(await p.pc.createOffer());
        p.send({ t: "desc", d: p.pc.localDescription.toJSON() });
        await waitState(p.pc, "connected", 15000);
        if (ch.readyState !== "open") await waitEvent(ch, "open", 10000);
        a.eq(ch.id % 2, 1, "offerer is DTLS server: odd stream id");
        await exercise(a, ch);
        const closed = waitEvent(ch, "close", 10000);
        ch.send("bye");
        await closed;
        a.eq(ch.readyState, "closed");
    } finally { p.pc.close(); p.sig.close(); p.child.stop(); }
}, { timeoutMs: 60000 });

t.test("child offers (DTLS server), parent answers; parent closes", async (a) => {
    const p = peer("offer");
    try {
        const dce = waitEvent(p.pc, "datachannel", 15000);
        await p.ready;
        const ch = (await dce).channel;
        if (ch.readyState !== "open") await waitEvent(ch, "open", 10000);
        a.eq(ch.label, "from-child");
        a.eq(ch.id % 2, 1, "the offering child is the DTLS server: odd stream id");
        await exercise(a, ch);
        const closed = waitEvent(ch, "close", 10000);
        ch.close();
        await closed;
        a.eq(ch.readyState, "closed");
    } finally { p.pc.close(); p.sig.close(); p.child.stop(); }
}, { timeoutMs: 60000 });

await t.run();
