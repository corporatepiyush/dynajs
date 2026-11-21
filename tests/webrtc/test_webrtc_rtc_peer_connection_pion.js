// slots: 2
// covers: dyna:webrtc.RTCPeerConnection -- interop with Pion (github.com/pion/webrtc v4), an independent WebRTC stack,
// in a second process over loopback UDP. Peer source: tests/webrtc/pion_peer (build: go build -o <bin> . in that
// directory); the suite runs when $PION_PEER names the built binary and skips otherwise. Matrix proven here:
// dynajs offers / Pion answers as DTLS client (dynajs is DTLS server), dynajs offers / Pion answers as DTLS server
// (dynajs is DTLS client), Pion offers / dynajs answers. Each pair: ICE + DTLS handshake, ordered, unordered and
// partially reliable (maxRetransmits 0) channels echoing strings, empty and binary messages, a 1 MiB message, a
// TypeError one byte past maxMessageSize, and close in both directions. Pion advertises a=max-message-size:1073741823
// but cannot reassemble a message larger than its 1 MiB SCTP receive buffer (a 4 MiB message stalls at a zero
// window), so 1 MiB is the largest size this pair can prove.
// RFC 9653 zero checksum: with PION_ZCSUM=1 (Pion's EnableSCTPZeroChecksum) both sides advertise EDMID 1 and both
// directions carry zero checksums (offer and answer side); without it dynajs never sends a zero checksum.
// slots: 2
import { RTCPeerConnection } from "dyna:webrtc";
import { UDPSocket } from "dyna:net";
import { getEnv } from "dyna:sys";
import { suite } from "../lib/kit.js";
import { spawnHelper } from "../lib/helper.js";
import { LO, waitEvent, waitState, delay } from "./rtc_util.js";

const t = suite("webrtc_pion");
const PEER = getEnv("PION_PEER") || "";
const BIG = 1 << 20;

function pion(role, dtls, env = "") {
    const sig = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const pc = new RTCPeerConnection(LO);
    let peerPort = 0;
    const queue = [];
    const send = (o) => {
        if (!peerPort) { queue.push(o); return; }
        sig.send(JSON.stringify(o), "127.0.0.1", peerPort);
    };
    const ready = new Promise((res) => {
        sig.start({
            message: async (d, from) => {
                const m = JSON.parse(new TextDecoder().decode(d));
                if (m.t === "hello") {
                    peerPort = from.port;
                    for (const q of queue.splice(0)) send(q);
                    res();
                } else if (m.t === "desc") {
                    await pc.setRemoteDescription(m.d);
                    if (m.d.type === "offer") {
                        await pc.setLocalDescription(await pc.createAnswer());
                        send({ t: "desc", d: pc.localDescription.toJSON() });
                    }
                } else if (m.t === "cand") {
                    await pc.addIceCandidate(m.c || null).catch(() => {});
                }
            },
        });
    });
    pc.onicecandidate = (e) => { if (e.candidate) send({ t: "cand", c: e.candidate.toJSON() }); };
    const child = spawnHelper((env ? "env " + env + " " : "") + PEER + " " + sig.port + " " + role + " " + dtls);
    return { pc, sig, child, ready, send, close() { pc.close(); sig.close(); child.stop(); } };
}

function echoAll(ch, msgs, ms = 20000) {
    return new Promise((res, rej) => {
        const got = [];
        const tm = setTimeout(() => rej(new Error(ch.label + ": echoed " + got.length + "/" + msgs.length)), ms);
        ch.onmessage = (e) => { got.push(e.data); if (got.length === msgs.length) { clearTimeout(tm); res(got); } };
        for (const m of msgs) ch.send(m);
    });
}

function remoteMax(pc) {
    const m = /a=max-message-size:(\d+)/.exec(pc.remoteDescription.sdp);
    return m ? +m[1] : 65536;
}

async function exercise(a, p, chans) {
    const max = p.pc.sctp.maxMessageSize;
    a.eq(max, Math.min(remoteMax(p.pc), max), "maxMessageSize honours the remote a=max-message-size");
    a.ok(max > 0 && max <= remoteMax(p.pc));
    for (const ch of chans) {
        ch.binaryType = "arraybuffer";
        const msgs = [];
        for (let i = 0; i < 50; i++) msgs.push(ch.label + i);
        if (ch.ordered && ch.maxRetransmits === null) {
            const big = new Uint8Array(Math.min(max, BIG));
            for (let i = 0; i < big.length; i++) big[i] = (i * 7 + 3) & 255;
            msgs.push("", new Uint8Array([9, 8, 7]), big);
            const got = await echoAll(ch, msgs);
            for (let i = 0; i < 50; i++) a.eq(got[i], ch.label + i, ch.label + " ordered echo");
            a.eq(got[50], "");
            a.deep(Array.from(new Uint8Array(got[51])), [9, 8, 7]);
            const rb = new Uint8Array(got[52]);
            a.eq(rb.length, big.length, "1 MiB message");
            let same = true;
            for (let i = 0; i < rb.length; i++) if (rb[i] !== big[i]) { same = false; break; }
            a.ok(same, ch.label + ": 1 MiB message intact");
            a.throws(() => ch.send(new Uint8Array(max + 1)), TypeError);
        } else {
            const got = await echoAll(ch, msgs);
            a.deep(got.slice().sort(), msgs.slice().sort(), ch.label + ": every message echoed (loopback, no loss)");
        }
    }
}

function needPeer(a) { if (!PEER) a.skip("set PION_PEER to the built tests/webrtc/pion_peer binary"); }

for (const dtls of ["dtls-client", "dtls-server"]) {
    t.test("dynajs offers, Pion answers as " + dtls + ": handshake, three channel kinds, max size, close", async (a) => {
        needPeer(a);
        const p = pion("answer", dtls);
        try {
            await p.ready;
            const chans = [p.pc.createDataChannel("ordered"), p.pc.createDataChannel("unordered", { ordered: false }),
                p.pc.createDataChannel("partial", { ordered: false, maxRetransmits: 0 })];
            await p.pc.setLocalDescription(await p.pc.createOffer());
            p.send({ t: "desc", d: p.pc.localDescription.toJSON() });
            await waitState(p.pc, "connected", 15000);
            for (const ch of chans) if (ch.readyState !== "open") await waitEvent(ch, "open", 10000);
            const want = dtls === "dtls-client" ? "passive" : "active";
            a.match(p.pc.localDescription.sdp, /a=setup:actpass/);
            a.match(p.pc.remoteDescription.sdp, dtls === "dtls-client" ? /a=setup:active/ : /a=setup:passive/, "Pion answered " + want);
            for (const ch of chans) a.eq(ch.id % 2, dtls === "dtls-client" ? 1 : 0, ch.label + ": id parity follows the DTLS role");
            const st = await p.pc.getStats();
            let tr = null;
            for (const r of st.values()) if (r.type === "transport") tr = r;
            a.must(tr, "transport stats");
            a.eq(tr.dtlsState, "connected");
            a.eq(tr.dtlsRole, dtls === "dtls-client" ? "server" : "client");
            await exercise(a, p, chans);
            const c0 = waitEvent(chans[0], "close", 10000);
            chans[0].send("bye");
            await c0;
            const c1 = waitEvent(chans[1], "close", 10000);
            chans[1].close();
            await c1;
            a.eq(chans[0].readyState, "closed");
            a.eq(chans[1].readyState, "closed");
            a.eq(chans[2].readyState, "open", "closing two channels leaves the third open");
        } finally { p.close(); }
    }, { timeoutMs: 60000 });
}

t.test("Pion offers three channels, dynajs answers (DTLS client): properties, echo, max size, close", async (a) => {
    needPeer(a);
    const p = pion("offer", "auto");
    try {
        const got = new Map();
        const all = new Promise((res) => {
            p.pc.ondatachannel = (e) => { got.set(e.channel.label, e.channel); if (got.size === 3) res(); };
        });
        await p.ready;
        await waitState(p.pc, "connected", 15000);
        await Promise.race([all, delay(10000).then(() => { throw new Error("got " + got.size + " channels"); })]);
        const o = got.get("ordered"), u = got.get("unordered"), pr = got.get("partial");
        a.eq(o.ordered, true);
        a.eq(u.ordered, false);
        a.eq(pr.ordered, false);
        a.eq(pr.maxRetransmits, 0);
        a.eq(o.maxRetransmits, null);
        a.match(p.pc.localDescription.sdp, /a=setup:active/);
        for (const ch of [o, u, pr]) {
            if (ch.readyState !== "open") await waitEvent(ch, "open", 10000);
            a.eq(ch.id % 2, 1, "Pion offered and is DTLS server: odd ids");
        }
        await exercise(a, p, [o, u, pr]);
        const c = waitEvent(o, "close", 10000);
        o.close();
        await c;
        const c2 = waitEvent(u, "close", 10000);
        u.send("bye");
        await c2;
        a.eq(o.readyState, "closed");
        a.eq(u.readyState, "closed");
    } finally { p.close(); }
}, { timeoutMs: 60000 });

async function sctpStats(pc) {
    for (const r of (await pc.getStats()).values()) if (r.type === "sctp-transport") return r;
    return null;
}

for (const [role, zc] of [["answer", true], ["offer", true], ["answer", false]]) {
    t.test("RFC 9653 zero checksum, Pion " + role + "s, Pion " + (zc ? "enabled" : "disabled"), async (a) => {
        needPeer(a);
        const p = pion(role, "auto", zc ? "PION_ZCSUM=1" : "");
        try {
            await p.ready;
            let ch;
            if (role === "answer") {
                ch = p.pc.createDataChannel("ordered");
                await p.pc.setLocalDescription(await p.pc.createOffer());
                p.send({ t: "desc", d: p.pc.localDescription.toJSON() });
            } else {
                ch = (await waitEvent(p.pc, "datachannel", 15000)).channel;
            }
            await waitState(p.pc, "connected", 15000);
            if (ch.readyState !== "open") await waitEvent(ch, "open", 10000);
            const msgs = [];
            for (let i = 0; i < 200; i++) msgs.push("z" + i + "-".repeat(i * 5));
            const got = await echoAll(ch, msgs);
            a.deep(got, msgs, "200 messages echoed intact");
            const s = await sctpStats(p.pc);
            a.must(s, "sctp-transport stats");
            a.eq(s.zeroChecksumNegotiated, zc, "negotiated");
            if (zc) {
                a.ok(s.zeroChecksumPacketsSent > 50, "dynajs sent zero checksums: " + s.zeroChecksumPacketsSent);
                a.ok(s.zeroChecksumPacketsReceived > 50, "Pion sent zero checksums that dynajs accepted: " + s.zeroChecksumPacketsReceived);
            } else {
                a.eq(s.zeroChecksumPacketsSent, 0, "no zero checksum towards a peer that did not advertise it");
            }
        } finally { p.close(); }
    }, { timeoutMs: 60000 });
}

await t.run();
