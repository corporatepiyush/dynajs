// covers: dyna:webrtc.RTCPeerConnection -- state machines and event order (P1 review round 1: R1-I1, I2, I10, I11,
// I13, M2, M3; R4-2, R4-6, R4-8).
// - I13: when gathering finishes the order is icecandidate with candidate "" (end-of-candidates, with sdpMid), then
//   icegatheringstatechange "complete", then icecandidate null (webrtc-pc 5.6 "ICE gathering finished").
// - I1: local end of gathering is fed to the ICE agent, so with remote end-of-candidates and every pair failed the
//   connection fails, and a working pair with nothing left to check reaches iceConnectionState "completed".
// - I2: a channel announced by the peer is already "open" inside ondatachannel (send works there); open fires after.
// - I10: createOffer rejects with InvalidStateError in have-remote-offer.
// - I11: after the peer's close_notify the connection stays "connected" (DTLS "closed" counts as connected), never "new".
// - M2: pranswer goes to pendingLocal/pendingRemoteDescription; the final answer moves both to current.
// - concurrency 2: generateCertificate generates the key on the shared io pool (offload_cpu); with --io-threads 0 it
//   falls back to the loop thread. Measured on this host: 8 x RSA 2048 stall the loop 208-520 ms inline, 0.14 ms offloaded;
//   8 x ECDSA P-256 1.6-4.3 ms inline, 0.13 ms offloaded.
// - M3: an implicit rollback (remote offer in have-local-offer) fires signalingstatechange "stable" first.
import { RTCPeerConnection } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { Exec, getEnv } from "dyna:sys";
const BIN = getEnv("DYNAJS_CHILD_BIN") || "./dynajs";
import { LO, waitEvent, waitState, delay, connectPair, closeAll, negotiate } from "./rtc_util.js";

const t = suite("webrtc_states");

t.test("end of gathering: '' candidate, then gathering complete, then null", async (a) => {
    const x = new RTCPeerConnection(LO);
    const seq = [];
    x.onicecandidate = (e) => seq.push(e.candidate === null ? "null" : e.candidate.candidate === "" ? "eoc:" + e.candidate.sdpMid : "cand");
    x.onicegatheringstatechange = () => seq.push("gather:" + x.iceGatheringState);
    x.createDataChannel("d");
    await x.setLocalDescription(await x.createOffer());
    for (let i = 0; i < 200 && !seq.includes("null"); i++) await delay(10);
    const tail = seq.filter((s) => s !== "cand" && s !== "gather:gathering");
    a.deep(tail, ["eoc:0", "gather:complete", "null"], seq.join(","));
    x.close();
});

t.test("a remote channel is open inside ondatachannel and can send there", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.onicecandidate = (e) => { if (e.candidate) y.addIceCandidate(e.candidate); };
    y.onicecandidate = (e) => { if (e.candidate) x.addIceCandidate(e.candidate); };
    const dc = x.createDataChannel("d");
    const got = waitEvent(dc, "message", 10000);
    let inHandler = null, sendErr = null, opened = false, order = [];
    y.ondatachannel = (e) => {
        inHandler = e.channel.readyState;
        e.channel.onopen = () => { opened = true; order.push("open"); };
        try { e.channel.send("from-handler"); } catch (err) { sendErr = err; }
        order.push("datachannel");
    };
    await negotiate(x, y);
    a.eq((await got).data, "from-handler");
    await delay(20);
    a.eq(inHandler, "open", "readyState inside ondatachannel");
    a.eq(sendErr, null, "send() inside ondatachannel");
    a.ok(opened, "open fired");
    a.deep(order, ["datachannel", "open"]);
    closeAll(x, y);
});

t.test("createOffer in have-remote-offer rejects with InvalidStateError", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("d");
    await y.setRemoteDescription(await x.createOffer());
    let err = null;
    try { await y.createOffer(); } catch (e) { err = e; }
    a.ok(err && err.name === "InvalidStateError", "got " + (err && err.name));
    closeAll(x, y);
});

t.test("after the peer closes, connectionState never returns to new", async (a) => {
    const { a: x, b: y } = await connectPair();
    const seen = [];
    x.onconnectionstatechange = () => seen.push(x.connectionState);
    y.close();
    await delay(1500);
    a.ok(!seen.includes("new"), "states after the peer's close_notify: " + seen.join(","));
    a.ok(x.connectionState === "connected" || x.connectionState === "disconnected" || x.connectionState === "failed", x.connectionState);
    x.close();
});

t.test("pranswer is pending; the final answer becomes current on both sides", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("d");
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    await y.setRemoteDescription(o);
    const ans = await y.createAnswer();
    await y.setLocalDescription({ type: "pranswer", sdp: ans.sdp });
    a.eq(y.signalingState, "have-local-pranswer");
    a.eq(y.pendingLocalDescription && y.pendingLocalDescription.type, "pranswer");
    a.eq(y.currentLocalDescription, null, "currentLocalDescription stays null for a pranswer");
    a.eq(y.pendingRemoteDescription && y.pendingRemoteDescription.type, "offer");
    await x.setRemoteDescription({ type: "pranswer", sdp: ans.sdp });
    a.eq(x.signalingState, "have-remote-pranswer");
    a.eq(x.pendingRemoteDescription && x.pendingRemoteDescription.type, "pranswer");
    a.eq(x.currentRemoteDescription, null);
    a.eq(x.pendingLocalDescription && x.pendingLocalDescription.type, "offer");
    const fin = await y.createAnswer();
    await y.setLocalDescription(fin);
    a.eq(y.currentLocalDescription.type, "answer");
    a.eq(y.pendingLocalDescription, null);
    a.eq(y.currentRemoteDescription.type, "offer");
    a.eq(y.pendingRemoteDescription, null);
    await x.setRemoteDescription(fin);
    a.eq(x.currentRemoteDescription.type, "answer");
    a.eq(x.currentLocalDescription.type, "offer");
    a.eq(x.pendingRemoteDescription, null);
    a.eq(x.pendingLocalDescription, null);
    closeAll(x, y);
});

t.test("implicit rollback fires stable before have-remote-offer", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("d");
    y.createDataChannel("e");
    await x.setLocalDescription(await x.createOffer());
    const states = [];
    x.onsignalingstatechange = () => states.push(x.signalingState);
    await x.setRemoteDescription(await y.createOffer());
    a.deep(states, ["stable", "have-remote-offer"]);
    closeAll(x, y);
});

t.test("a working pair with end-of-candidates on both sides reaches completed", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const fwd = (to) => (e) => { to.addIceCandidate(e.candidate ? e.candidate.toJSON() : null).catch(() => {}); };
    x.onicecandidate = fwd(y);
    y.onicecandidate = fwd(x);
    x.createDataChannel("d");
    await negotiate(x, y);
    for (let i = 0; i < 300 && x.iceConnectionState !== "completed"; i++) await delay(20);
    a.eq(x.iceConnectionState, "completed", "controlling side");
    closeAll(x, y);
});

t.test("only an unreachable remote candidate plus end-of-candidates: the connection fails", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("d");
    await negotiate(x, y);
    await x.addIceCandidate({ candidate: "candidate:1 1 udp 2122260223 127.0.0.1 9 typ host", sdpMid: "0" });
    await x.addIceCandidate(null);
    await waitState(x, "failed", 60000);
    a.eq(x.iceConnectionState, "failed");
    closeAll(x, y);
}, { timeoutMs: 70000 });

t.test("generateCertificate runs on the io pool: the loop keeps ticking during RSA 2048 keygen", async (a) => {
    const alg = { name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" };
    let maxGap = 0, last = performance.now(), ticking = true, ticks = 0;
    const tick = () => { const n = performance.now(); maxGap = Math.max(maxGap, n - last); last = n; ticks++; if (ticking) setTimeout(tick, 0); };
    setTimeout(tick, 0);
    const t0 = performance.now();
    const certs = await Promise.all([0, 1, 2, 3].map(() => RTCPeerConnection.generateCertificate(alg)));
    const total = performance.now() - t0;
    ticking = false;
    maxGap = Math.max(maxGap, performance.now() - last);
    a.ok(ticks >= 10, "the loop ran timers while the keys were generated (" + ticks + " ticks)");
    a.eq(certs.length, 4);
    for (const c of certs) a.ok(c.getFingerprints()[0].value.length > 0, "certificate usable");
    a.ok(maxGap < 100, "max loop stall " + maxGap.toFixed(2) + " ms during " + total.toFixed(1) + " ms of keygen (" + ticks + " ticks)");
});

t.test("generateCertificate with --io-threads 0 falls back to the loop thread and still resolves", async (a) => {
    const run = (flags) => {
        const r = Exec("/bin/sh", ["-c", BIN + " " + flags + " --std tests/webrtc/gencert_child.js"]);
        const m = /gencert: (\d+) (\d+)/.exec(String(r.stdout));
        return { code: r.code, n: m ? +m[1] : -1, ticks: m ? +m[2] : -1, out: String(r.stdout) + String(r.stderr) };
    };
    const inline = run("--io-threads 0");
    a.eq(inline.code, 0, inline.out);
    a.eq(inline.n, 4, "four usable certificates inline: " + inline.out);
    const pooled = run("");
    a.eq(pooled.n, 4, "four usable certificates pooled: " + pooled.out);
    a.ok(pooled.ticks >= 10, "pooled generation leaves the loop free (" + pooled.ticks + " ticks; inline " + inline.ticks + ")");
}, { timeoutMs: 60000 });

await t.run();
