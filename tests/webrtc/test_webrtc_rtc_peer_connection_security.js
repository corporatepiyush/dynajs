// covers: dyna:webrtc.RTCPeerConnection -- off-path attacks on the transport (P1 review round 1, R2 + R1 + R4).
// - R2-C2 / R1-C1: DTLS is fed only from addresses that authenticated with ICE (a valid pair or an authenticated
//   Binding request, RFC 8445 12.2 / RFC 7675 5.1). A third socket spraying a 15-byte plaintext fatal alert
//   (15 fe fd ... 02 28) at both peers during setup must not fail the connection.
//   R2b-C2: the same spray keeps going after a valid pair exists (y checked x; x has no answer yet, so y's ClientHello
//   is held at x) and through the handshake; only the authenticated 5-tuple feeds DTLS, so both connect. Each alert
//   carries a fresh epoch-0 sequence number so DTLS replay detection (RFC 6347 4.1.2.6) cannot be what saves it.
// - R2-C3: a bare 20-byte Binding request (no FINGERPRINT, which RFC 8445 7.2.2 requires on checks) gets no reply.
//   Requests that do carry FINGERPRINT but fail auth still get the RFC 8489 9.1.3 400/401, short (<= 36 B), without
//   XOR-MAPPED-ADDRESS and rate limited to 20/s (checked in test_rtc_core.c test_ice_auth_replies).
// - R4-15: the remote fingerprint is checked in the certificate verify callback, so a mismatch aborts the handshake
//   with a fatal bad_certificate alert and the peer fails as well instead of reporting connected.
// - R4-3: after consent expiry (30 s, RFC 7675) the connection is failed and nothing more is sent on that 5-tuple.
// - R4-10: a peer whose check succeeded may send its ClientHello before our answer is applied (libwebrtc starts DTLS on
//   the first writable pair). The record comes from an authenticated source but we have no SSL yet (role unknown), so
//   it is held (up to 4 x 1500 B) and replayed when setRemoteDescription sets the role; the reply needs no resend.
import { RTCPeerConnection } from "dyna:webrtc";
import { UDPSocket } from "dyna:net";
import { suite } from "../lib/kit.js";
import { hostScale } from "../lib/hostscale.js";
import { LO, waitState, waitEvent, delay, negotiate, closeAll, LossyRelay, trickle } from "./rtc_util.js";
import { Writer } from "./turn_server.js";
import { RandomBytes } from "dyna:crypto";

const t = suite("webrtc_security");
const ALERT = new Uint8Array([0x15, 0xfe, 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x02, 0x28]);

function hostOf(init) {
    const m = /^candidate:\S+ \d+ udp \d+ (\S+) (\d+) typ host/.exec(init && init.candidate || "");
    return m ? { address: m[1], port: +m[2] } : null;
}

t.test("a spoofed DTLS fatal alert from a third address does not fail the handshake", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const held = [];
    x.addEventListener("icecandidate", (e) => { if (e.candidate) held.push([y, e.candidate.toJSON()]); });
    y.addEventListener("icecandidate", (e) => { if (e.candidate) held.push([x, e.candidate.toJSON()]); });
    const spoof = new UDPSocket({ port: 0, host: "127.0.0.1" });
    x.createDataChannel("s");
    const o = await x.createOffer(); await x.setLocalDescription(o); await y.setRemoteDescription(o);
    const an = await y.createAnswer(); await y.setLocalDescription(an); await x.setRemoteDescription(an);
    await delay(50);
    let sent = 0;
    for (const [, c] of held) { const h = hostOf(c); if (h) for (let k = 0; k < 4; k++) { spoof.send(ALERT, h.address, h.port); sent++; } }
    await delay(20);
    for (const [pc, c] of held) pc.addIceCandidate(c);
    a.ok(sent >= 8, "alerts reached both peers before any ICE check: " + sent);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    a.eq(x.connectionState, "connected");
    a.eq(y.connectionState, "connected");
    spoof.close();
    closeAll(x, y);
});

t.test("a bare 20-byte Binding request (no USERNAME, no FINGERPRINT) is not answered at all", async (a) => {
    const x = new RTCPeerConnection(LO);
    let target = null;
    x.addEventListener("icecandidate", (e) => { if (!target && e.candidate) target = hostOf(e.candidate.toJSON()); });
    x.createDataChannel("s");
    await x.setLocalDescription(await x.createOffer());
    for (let i = 0; i < 100 && !target; i++) await delay(10);
    a.ok(target, "got the host candidate");
    const req = new Uint8Array(20);
    req[1] = 1; req[4] = 0x21; req[5] = 0x12; req[6] = 0xa4; req[7] = 0x42;
    let replies = 0;
    const probe = new UDPSocket({ port: 0, host: "127.0.0.1" });
    probe.start({ message: () => { replies++; } });
    for (let i = 0; i < 60; i++) { req[8] = i; probe.send(req, target.address, target.port); }
    await delay(300);
    a.eq(replies, 0, "no reflection for checks without FINGERPRINT (RFC 8445 7.2.2)");
    probe.close();
    x.close();
});

t.test("consent expiry: failed at ~30 s and nothing more is sent", async (a) => {
    const relay = new LossyRelay({ loss: 0, seed: 3 });
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    relay.pair(x, y);
    trickle(x, y, relay.rewrite());
    const dc = x.createDataChannel("s");
    await negotiate(x, y);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    relay.loss = 1;
    const t0 = Date.now();
    await waitState(x, "failed", 40000);
    const took = Date.now() - t0;
    // wall-clock: consent freshness is a 30 s wall-clock timer (RFC 7675 5.1); the lower bound is exact, the upper one scales with the host.
    a.ok(took > 25000 && took < 36000 * hostScale(), "failed after " + took + " ms");
    const sent = async () => { let n = 0; (await x.getStats()).forEach((s) => { if (s.type === "transport") n = s.packetsSent; }); return n; };
    const p0 = await sent();
    try { dc.send("after"); } catch (e) {}
    await delay(1500);
    a.eq(await sent(), p0, "no packets leave after consent expiry");
    closeAll(x, y);
    relay.close();
});

t.test("a fingerprint mismatch fails the handshake with a fatal alert, so the peer fails too (R4-15)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.onicecandidate = (e) => { if (e.candidate) y.addIceCandidate(e.candidate); };
    y.onicecandidate = (e) => { if (e.candidate) x.addIceCandidate(e.candidate); };
    x.createDataChannel("s");
    const o = await x.createOffer(); await x.setLocalDescription(o); await y.setRemoteDescription(o);
    const an = await y.createAnswer(); await y.setLocalDescription(an);
    const bad = an.sdp.replace(/a=fingerprint:sha-256 ([0-9A-F]{2})/, (m, h) => "a=fingerprint:sha-256 " + (h === "00" ? "01" : "00"));
    a.ok(bad !== an.sdp, "fingerprint rewritten");
    await x.setRemoteDescription({ type: "answer", sdp: bad });
    const ys = [];
    y.onconnectionstatechange = () => ys.push(y.connectionState);
    await waitState(x, "failed", 10000);
    for (let i = 0; i < 100 && y.connectionState !== "failed"; i++) await delay(20);
    a.eq(y.connectionState, "failed", "the peer learns of the rejection from the alert: " + ys.join(","));
    a.ok(!ys.includes("connected"), "the peer never reports connected");
    closeAll(x, y);
});

function clientHello() {
    const ext = [0x00, 0x0a, 0x00, 0x06, 0x00, 0x04, 0x00, 0x1d, 0x00, 0x17,
        0x00, 0x0b, 0x00, 0x02, 0x01, 0x00,
        0x00, 0x0d, 0x00, 0x08, 0x00, 0x06, 0x04, 0x03, 0x08, 0x04, 0x04, 0x01,
        0x00, 0x17, 0x00, 0x00,
        0xff, 0x01, 0x00, 0x01, 0x00];
    const body = [0xfe, 0xfd, ...RandomBytes(32), 0x00, 0x00, 0x00, 0x04, 0xc0, 0x2b, 0xc0, 0x2f, 0x01, 0x00,
        ext.length >> 8, ext.length & 255, ...ext];
    const n = body.length;
    const hs = [0x01, 0, n >> 8, n & 255, 0, 0, 0, 0, 0, 0, n >> 8, n & 255, ...body];
    return new Uint8Array([0x16, 0xfe, 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, hs.length >> 8, hs.length & 255, ...hs]);
}

t.test("an early ClientHello from an authenticated source is held and answered once the answer sets the role (R4-10)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const raw = new UDPSocket({ port: 0, host: "127.0.0.1" });
    try {
        let target = null;
        x.addEventListener("icecandidate", (e) => { if (!target && e.candidate) target = hostOf(e.candidate.toJSON()); });
        x.createDataChannel("s");
        const o = await x.createOffer();
        await x.setLocalDescription(o);
        for (let i = 0; i < 100 && !target; i++) await delay(10);
        a.must(target, "got the host candidate");
        const lufrag = /a=ice-ufrag:(\S+)/.exec(o.sdp)[1], lpwd = /a=ice-pwd:(\S+)/.exec(o.sdp)[1];
        const stunOk = [], dtls = [];
        raw.start({ message: (d) => {
            if (d[0] === 0x01 && d[1] === 0x01) stunOk.push(d);
            else if (d[0] >= 20 && d[0] <= 63) dtls.push(d);
        } });
        const req = new Writer(0x0001, RandomBytes(12))
            .add(0x0006, new TextEncoder().encode(lufrag + ":rawU"))
            .u32(0x0024, 0x6e7f1eff)
            .add(0x8029, RandomBytes(8))
            .build(new TextEncoder().encode(lpwd), true);
        for (let i = 0; i < 50 && !stunOk.length; i++) { if (i % 10 === 0) raw.send(req, target.address, target.port); await delay(10); }
        a.ok(stunOk.length > 0, "the authenticated early check is answered (RFC 8445 7.3)");
        raw.send(clientHello(), target.address, target.port);
        await delay(200);
        a.eq(dtls.length, 0, "no DTLS reply while the role is unknown");
        await y.setRemoteDescription(o);
        const an = await y.createAnswer();
        y.close();
        const sdp = an.sdp.replace(/a=ice-ufrag:\S+/, "a=ice-ufrag:rawU").replace(/a=ice-pwd:\S+/, "a=ice-pwd:rawpasswordrawpassword0123")
            .replace(/a=setup:\S+/, "a=setup:active").replace(/a=candidate:[^\r\n]*\r\n/g, "");
        await x.setRemoteDescription({ type: "answer", sdp });
        for (let i = 0; i < 100 && !dtls.length; i++) await delay(10);
        a.ok(dtls.length > 0, "the held ClientHello is processed after the answer, without a resend");
        a.eq(dtls.length && dtls[0][0], 22, "the reply is a handshake record");
        a.eq(dtls.length && dtls[0][13], 2, "ServerHello (handshake type 2): the hold kept the whole ClientHello");
    } finally { raw.close(); x.close(); y.close(); }
});
t.test("spoofed DTLS alerts from a third address after a valid pair exists (handshake pending) do not fail it (R2b-C2)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const spoof = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const xc = [], yc = [], targets = [];
    let spraying = true, sent = 0;
    try {
        x.addEventListener("icecandidate", (e) => { if (e.candidate && e.candidate.candidate) xc.push(e.candidate.toJSON()); });
        y.addEventListener("icecandidate", (e) => { if (e.candidate && e.candidate.candidate) yc.push(e.candidate.toJSON()); });
        const dc = x.createDataChannel("s");
        const o = await x.createOffer(); await x.setLocalDescription(o);
        await y.setRemoteDescription(o);
        const an = await y.createAnswer(); await y.setLocalDescription(an);
        for (let i = 0; i < 100 && (!xc.length || !yc.length); i++) await delay(10);
        for (const c of xc) { await y.addIceCandidate(c); const h = hostOf(c); if (h) targets.push(h); }
        for (const c of yc) { const h = hostOf(c); if (h) targets.push(h); }
        a.ok(targets.length >= 2, "host candidates of both peers: " + targets.length);
        const spray = (async () => {
            while (spraying) {
                const al = ALERT.slice();
                al[8] = (sent >> 16) & 255; al[9] = (sent >> 8) & 255; al[10] = sent & 255;
                for (const h of targets) { spoof.send(al, h.address, h.port); sent++; }
                await delay(5);
            }
        })();
        let pair = null;
        for (let i = 0; i < 200 && !pair; i++) {
            const st = await y.getStats();
            for (const r of st.values()) if (r.type === "candidate-pair" && r.state === "succeeded") pair = r;
            if (!pair) await delay(10);
        }
        a.must(pair, "y has a valid pair while x still lacks the answer, so y's ClientHello is held at x");
        await delay(150);
        const before = sent;
        await x.setRemoteDescription(an);
        for (const c of yc) await x.addIceCandidate(c);
        await Promise.all([waitState(x, "connected", 15000), waitState(y, "connected", 15000)]);
        await delay(100);
        spraying = false;
        await spray;
        a.ok(before >= 20 && sent > before, "alerts sprayed after the valid pair and during the handshake: " + before + " then " + sent);
        a.eq(x.connectionState, "connected");
        a.eq(y.connectionState, "connected");
        if (dc.readyState !== "open") await waitEvent(dc, "open", 5000);
        a.eq(dc.readyState, "open", "the data channel opens over the spoofed 5-tuples' real peer");
    } finally { spraying = false; spoof.close(); closeAll(x, y); }
}, { timeoutMs: 30000 });


await t.run();
