// covers: dyna:webrtc.RTCPeerConnection -- renegotiation and ICE restart (P1 review round 1: R1-C2, R1-I3, R1-M1,
// R4-4, R4-16 and R4's untested sequences).
// - R1-C2: an answerer-initiated re-offer (actpass, same fingerprint) must not flip our DTLS role: RFC 8842 5.3 says
//   the answer keeps the previously negotiated roles; a flip would need a new DTLS association. Chrome's restartIce
//   re-offer used to make us answer setup:active while we were the DTLS server.
// - R1-I3: a remote ICE restart offer carrying candidates: they belong to the new session and must survive applying
//   our answer (the old code restarted the agent after adding them and lost them).
// - R4-4: restart while streaming: the previous pair keeps carrying data (old credentials stay valid) until a new
//   pair is selected, so the receiver never sees a gap above 1 s.
// - R1-M1: a missing a=setup means active in an offer and passive in an answer (RFC 4145 section 4).
// - R4-16: setup:holdconn parses (ignored, treated as absent) instead of failing the whole description.
// - R1-I5 / R1b-3: the ICE role is fixed by the initial offer/answer and is never redetermined, not even by an ICE
//   restart (RFC 8445 6.1.1; only a full/lite change or a 487 conflict moves it): an answerer-initiated re-offer, with
//   or without restart, leaves the original offerer controlling; the kernel resolves conflicts with 487.
// - R1b-4: an ICE restart is staged until its description is final (the answer); rollback, explicit or implicit
//   (glare), drops the staging, so the answer to the peer's offer carries the current credentials and the wanted restart
//   is offered again (JSEP 5.7/5.10, W3C [[LocalIceCredentialsToReplace]]). An answer that changes the credentials of
//   an offer that did not restart is an InvalidAccessError.
// - R1b-5: setRemoteDescription validates everything before it changes anything (no implicit rollback on failure).
// - R1b-6: an operation whose own events close the connection never settles its promise.
// - R1b-9: while a remote restart offer is pending, addIceCandidate takes candidates of both generations (the current
//   one from currentRemoteDescription, the new one from pendingRemoteDescription); any other ufrag is an OperationError.
import { RTCPeerConnection } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { LO, connectPair, waitEvent, waitState, delay, closeAll, negotiate } from "./rtc_util.js";

const t = suite("webrtc_renegotiation");
const setupOf = (sdp) => (/a=setup:(\w+)/.exec(sdp) || [])[1];

async function gathered(pc) {
    if (pc.iceGatheringState !== "complete") await waitEvent(pc, "icegatheringstatechange", 5000, () => pc.iceGatheringState === "complete");
}

t.test("answerer-initiated ICE-restart re-offer keeps our DTLS role", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    const xRole = setupOf(x.currentLocalDescription.sdp), yRole = setupOf(y.currentLocalDescription.sdp);
    a.eq(xRole, "actpass", "x offered actpass");
    a.eq(yRole, "active", "y answered active, so x is the DTLS server");
    y.restartIce();
    const o = await y.createOffer();
    a.eq(setupOf(o.sdp), "actpass", "re-offer is actpass");
    await y.setLocalDescription(o);
    await x.setRemoteDescription(o);
    const ans = await x.createAnswer();
    a.eq(setupOf(ans.sdp), "passive", "x keeps the server role in its answer");
    await x.setLocalDescription(ans);
    await y.setRemoteDescription(ans);
    await delay(300);
    const got = waitEvent(rdc, "message", 5000);
    dc.send("after re-offer");
    a.eq((await got).data, "after re-offer");
    a.eq(x.connectionState, "connected");
    closeAll(x, y);
});

t.test("remote restart offer with candidates: they survive our answer and the session reconnects", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    let gate = true;
    x.addEventListener("icecandidate", (e) => { if (e.candidate) y.addIceCandidate(e.candidate).catch(() => {}); });
    y.addEventListener("icecandidate", (e) => { if (gate && e.candidate) x.addIceCandidate(e.candidate).catch(() => {}); });
    const dc = x.createDataChannel("r");
    const rdcP = waitEvent(y, "datachannel");
    await negotiate(x, y);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    const rdc = (await rdcP).channel;
    if (dc.readyState !== "open") await waitEvent(dc, "open");
    gate = false;
    y.restartIce();
    await y.setLocalDescription(await y.createOffer());
    await gathered(y);
    const offer = y.localDescription;
    a.ok(/a=candidate:/.test(offer.sdp), "the restart offer carries candidates");
    await x.setRemoteDescription(offer);
    await x.setLocalDescription(await x.createAnswer());
    await y.setRemoteDescription(x.localDescription);
    await delay(500);
    let hosts = 0;
    (await x.getStats()).forEach((s) => { if (s.type === "remote-candidate" && s.candidateType === "host") hosts++; });
    a.ok(hosts > 0, "x still knows y's signalled host candidates after the restart: " + hosts);
    const got = waitEvent(rdc, "message", 5000);
    dc.send("post-restart");
    a.eq((await got).data, "post-restart");
    closeAll(x, y);
});

t.test("restart while streaming: no receive gap above 1 s", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    let last = Date.now(), maxGap = 0, n = 0;
    rdc.onmessage = () => { const now = Date.now(); maxGap = Math.max(maxGap, now - last); last = now; n++; };
    let on = true;
    const pump = (async () => { while (on) { dc.send("x"); await delay(1); } })();
    await delay(200);
    x.restartIce();
    await x.setLocalDescription(await x.createOffer());
    await delay(150);
    await y.setRemoteDescription(x.localDescription);
    await delay(150);
    await y.setLocalDescription(await y.createAnswer());
    await delay(150);
    await x.setRemoteDescription(y.localDescription);
    await delay(1500);
    on = false;
    await pump;
    a.ok(n > 200, "messages flowed: " + n);
    a.ok(maxGap < 1000, "largest gap " + maxGap + " ms");
    a.eq(x.connectionState, "connected");
    closeAll(x, y);
});

t.test("explicit setup in a remote offer is honoured; a missing one means active (RFC 4145)", async (a) => {
    for (const [munge, want] of [[(s) => s.replace("a=setup:actpass", "a=setup:passive"), "active"],
        [(s) => s.replace("a=setup:actpass", "a=setup:active"), "passive"],
        [(s) => s.replace(/a=setup:actpass\r\n/, ""), "passive"],
        [(s) => s.replace("a=setup:actpass", "a=setup:holdconn"), "passive"]]) {
        const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
        x.createDataChannel("s");
        const o = await x.createOffer();
        await y.setRemoteDescription({ type: "offer", sdp: munge(o.sdp) });
        const ans = await y.createAnswer();
        a.eq(setupOf(ans.sdp), want);
        closeAll(x, y);
    }
});

t.test("a missing setup in a remote answer means passive, so we become the DTLS client", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("s");
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    await y.setRemoteDescription(o);
    const ans = await y.createAnswer();
    await x.setRemoteDescription({ type: "answer", sdp: ans.sdp.replace(/a=setup:\w+\r\n/, "") });
    const st = await x.getStats();
    let role = "";
    st.forEach((s) => { if (s.type === "transport") role = s.dtlsRole || ""; });
    a.ok(role === "client" || role === "", "dtlsRole " + role);
    closeAll(x, y);
});

t.test("ICE credentials outside RFC 8839 lengths are rejected", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("s");
    const o = await x.createOffer();
    for (const bad of [o.sdp.replace(/a=ice-ufrag:\S+/, "a=ice-ufrag:abc"), o.sdp.replace(/a=ice-pwd:\S+/, "a=ice-pwd:short"),
        o.sdp.replace(/a=ice-ufrag:\S+/, "a=ice-ufrag:ab$d")]) {
        let threw = false;
        try { await y.setRemoteDescription({ type: "offer", sdp: bad }); } catch (e) { threw = true; }
        a.ok(threw, "rejected: " + (/a=ice-(ufrag|pwd):\S+/.exec(bad) || [])[0]);
    }
    closeAll(x, y);
});

t.test("a re-offer from the answerer without ICE restart keeps the ICE roles (R1-I5)", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    const role = async (pc) => { for (const s of (await pc.getStats()).values()) if (s.type === "transport") return s.iceRole; return null; };
    a.eq(await role(x), "controlling");
    a.eq(await role(y), "controlled");
    await negotiate(y, x);
    a.eq(await role(x), "controlling", "original offerer keeps controlling after the peer's re-offer");
    a.eq(await role(y), "controlled");
    const got = waitEvent(rdc, "message", 5000);
    dc.send("still");
    a.eq((await got).data, "still");
    closeAll(x, y);
});
const ufragOf = (sdp) => (/a=ice-ufrag:(\S+)/.exec(sdp) || [])[1];
const iceRole = async (pc) => { for (const s of (await pc.getStats()).values()) if (s.type === "transport") return s.iceRole; return null; };
function connTrace(pc) { const seen = []; pc.addEventListener("connectionstatechange", () => seen.push(pc.connectionState)); return seen; }

t.test("an answerer-initiated ICE restart keeps both ICE roles (R1b-3, RFC 8445 6.1.1)", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    a.eq(await iceRole(x), "controlling");
    a.eq(await iceRole(y), "controlled");
    const before = ufragOf(y.currentLocalDescription.sdp);
    y.restartIce();
    await negotiate(y, x);
    a.ok(ufragOf(y.currentLocalDescription.sdp) !== before, "the re-offer restarted ICE");
    a.eq(await iceRole(x), "controlling", "the original offerer stays controlling through the peer's restart");
    a.eq(await iceRole(y), "controlled", "the restarting answerer stays controlled");
    for (let i = 0; i < 50; i++) { const st = await x.getStats(); let sel = false; for (const s of st.values()) if (s.type === "candidate-pair" && s.nominated && s.state === "succeeded") sel = true; if (sel) break; await delay(50); }
    const got = waitEvent(rdc, "message", 5000);
    dc.send("after restart");
    a.eq((await got).data, "after restart");
    a.eq(x.connectionState, "connected");
    for (let i = 0; i < 60 && y.iceConnectionState !== "completed"; i++) await delay(50);
    a.eq(y.iceConnectionState, "completed", "the restarted agent knows local gathering is done, so its checklist completes");
    closeAll(x, y);
});

t.test("glare: a restart offer rolled back by the peer's plain offer is answered with the current credentials and retried (R1b-4)", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    const xs = connTrace(x), ys = connTrace(y);
    const yCur = ufragOf(y.currentLocalDescription.sdp);
    y.restartIce();
    const yo = await y.createOffer();
    a.ok(ufragOf(yo.sdp) !== yCur, "y's offer carries new credentials");
    await y.setLocalDescription(yo);
    const xo = await x.createOffer();
    await x.setLocalDescription(xo);
    await y.setRemoteDescription(xo);
    a.eq(y.signalingState, "have-remote-offer", "the plain offer rolled y's restart offer back");
    const ya = await y.createAnswer();
    a.eq(ufragOf(ya.sdp), yCur, "the answer keeps y's current ufrag: the rolled-back restart is not half-applied");
    const nn = waitEvent(y, "negotiationneeded", 3000);
    nn.catch(() => {});
    await y.setLocalDescription(ya);
    await x.setRemoteDescription(ya);
    const got = waitEvent(rdc, "message", 5000);
    dc.send("after glare");
    a.eq((await got).data, "after glare");
    a.eq(xs.join(","), "", "x never left connected");
    a.eq(ys.join(","), "", "y never left connected");
    await nn;
    const yo2 = await y.createOffer();
    a.ok(ufragOf(yo2.sdp) !== yCur, "the wanted restart survives the rollback: the next offer restarts ICE");
    await y.setLocalDescription(yo2);
    await x.setRemoteDescription(yo2);
    const xa2 = await x.createAnswer();
    await x.setLocalDescription(xa2);
    await y.setRemoteDescription(xa2);
    const got2 = waitEvent(rdc, "message", 5000);
    dc.send("after retry");
    a.eq((await got2).data, "after retry");
    closeAll(x, y);
});

t.test("setLocalDescription(rollback) of a restart offer leaves ICE untouched and keeps the restart wanted (R1b-4)", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    const xs = connTrace(x);
    const cur = ufragOf(x.currentLocalDescription.sdp);
    x.restartIce();
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    await x.setLocalDescription({ type: "rollback" });
    a.eq(x.signalingState, "stable");
    await delay(200);
    const got = waitEvent(rdc, "message", 5000);
    dc.send("after rollback");
    a.eq((await got).data, "after rollback");
    const o2 = await x.createOffer();
    a.ok(ufragOf(o2.sdp) !== cur, "the next offer still restarts");
    a.eq(xs.join(","), "", "x never left connected");
    closeAll(x, y);
});

t.test("an answer that changes ICE credentials to an offer that did not restart is rejected and changes nothing (R1b-4, JSEP 5.10)", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    await y.setRemoteDescription(o);
    const ans = await y.createAnswer();
    const bad = ans.sdp.replace(/a=ice-ufrag:\S+/, "a=ice-ufrag:zzzz").replace(/a=ice-pwd:\S+/, "a=ice-pwd:zzzzzzzzzzzzzzzzzzzzzzzz");
    let err = null;
    try { await x.setRemoteDescription({ type: "answer", sdp: bad }); } catch (e) { err = e; }
    a.ok(err && err.name === "InvalidAccessError", "InvalidAccessError: " + (err && err.name));
    a.eq(x.signalingState, "have-local-offer", "still waiting for the real answer");
    await y.setLocalDescription(ans);
    await x.setRemoteDescription(ans);
    const got = waitEvent(rdc, "message", 5000);
    dc.send("still");
    a.eq((await got).data, "still");
    closeAll(x, y);
});

t.test("a setRemoteDescription that fails validation changes no state: no implicit rollback, no credential change (R1b-5)", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    const xCur = ufragOf(x.currentLocalDescription.sdp);
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    y.restartIce();
    const yo = await y.createOffer();
    const bad = yo.sdp.replace(/a=fingerprint:sha-256 \S+/, "a=fingerprint:sha-256 " + Array(32).fill("AB").join(":"));
    let err = null;
    try { await x.setRemoteDescription({ type: "offer", sdp: bad }); } catch (e) { err = e; }
    a.ok(err && err.name === "InvalidModificationError", "fingerprint change rejected: " + (err && err.name));
    a.eq(x.signalingState, "have-local-offer", "the failed offer did not roll x's local offer back");
    a.ok(x.pendingLocalDescription !== null, "x's pending offer is intact");
    await y.setRemoteDescription(o);
    const ya = await y.createAnswer();
    await y.setLocalDescription(ya);
    await x.setRemoteDescription(ya);
    a.eq(ufragOf(x.currentLocalDescription.sdp), xCur, "x's credentials did not change");
    const got = waitEvent(rdc, "message", 5000);
    dc.send("ok");
    a.eq((await got).data, "ok");
    closeAll(x, y);
});

t.test("close() from a signalingstatechange handler during setLocalDescription: the promise never settles (R1b-6)", async (a) => {
    const x = new RTCPeerConnection(LO);
    x.createDataChannel("c");
    const o = await x.createOffer();
    let settled = "";
    x.addEventListener("signalingstatechange", () => x.close());
    x.setLocalDescription(o).then(() => { settled = "resolved"; }, (e) => { settled = "rejected " + e.name; });
    await delay(200);
    a.eq(x.signalingState, "closed");
    a.eq(settled, "", "webrtc-pc chains abort when [[IsClosed]]: no settle after close");
    const y = new RTCPeerConnection(LO);
    const z = new RTCPeerConnection(LO);
    z.createDataChannel("c");
    const zo = await z.createOffer();
    let ys = "";
    y.addEventListener("signalingstatechange", () => y.close());
    y.setRemoteDescription(zo).then(() => { ys = "resolved"; }, (e) => { ys = "rejected " + e.name; });
    await delay(200);
    a.eq(ys, "", "setRemoteDescription closed mid-operation does not settle either");
    closeAll(y, z);
});

t.test("addIceCandidate during a remote ICE restart: the current generation's ufrag and the restart's both apply (R1b-9)", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    const yOld = ufragOf(y.currentLocalDescription.sdp);
    y.restartIce();
    const yo = await y.createOffer();
    await y.setLocalDescription(yo);
    await x.setRemoteDescription(yo);
    const yNew = ufragOf(yo.sdp);
    a.ok(yNew !== yOld, "restart offer");
    const cand = "candidate:9 1 udp 2122260223 127.0.0.1 9 typ host";
    let e1 = null, e2 = null, e3 = null;
    try { await x.addIceCandidate({ candidate: cand, sdpMid: "0", usernameFragment: yOld }); } catch (e) { e1 = e; }
    a.eq(e1, null, "a candidate of the generation still in currentRemoteDescription is accepted: " + (e1 && e1.name));
    try { await x.addIceCandidate({ candidate: cand, sdpMid: "0", usernameFragment: yNew }); } catch (e) { e2 = e; }
    a.eq(e2, null, "a candidate of the pending restart is accepted");
    try { await x.addIceCandidate({ candidate: cand, sdpMid: "0", usernameFragment: "nope" }); } catch (e) { e3 = e; }
    a.ok(e3 && e3.name === "OperationError", "an unknown ufrag is an OperationError: " + (e3 && e3.name));
    const xa = await x.createAnswer();
    await x.setLocalDescription(xa);
    await y.setRemoteDescription(xa);
    const got = waitEvent(rdc, "message", 5000);
    dc.send("after");
    a.eq((await got).data, "after");
    closeAll(x, y);
});


await t.run();
