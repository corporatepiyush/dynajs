// covers: integration dyna:webrtc.RTCDtlsTransport dyna:webrtc.RTCIceTransport -- sender/receiver/sctp transport objects, states, events, candidates, certificates
// dyna:webrtc RTCDtlsTransport and RTCIceTransport (webrtc-pc 5.5, 5.6) between two in-process RTCPeerConnections over
// loopback, written by hand from the intent of the WPT webrtc tests (RTCDtlsTransport-state, RTCDtlsTransport-
// getRemoteCertificates, RTCIceTransport, RTCRtpSender-transport, RTCSctpTransport-constructor), no download:
//   - sender.transport / receiver.transport are null until the transceiver is associated with an m= section, then the
//     one RTCDtlsTransport of the BUNDLE group: every sender, every receiver and pc.sctp.transport are the same object,
//     and its iceTransport is the one RTCIceTransport;
//   - states follow the connection: dtls new -> connecting -> connected with a statechange each, ice new -> checking ->
//     connected/completed and gatheringState new -> gathering -> complete with their events; each transport event fires before the matching
//     RTCPeerConnection event (webrtc-pc 5.6); close() moves both to
//     "closed" without events (webrtc-pc 4.4.1.6 close steps);
//   - getRemoteCertificates() is the peer's DER certificate whose SHA-256 is the a=fingerprint the peer signalled;
//     getLocal/RemoteParameters are the a=ice-ufrag/a=ice-pwd of each side; role is controlling for the offerer and
//     controlled for the answerer (RFC 8445 6.1.1), component "rtp"; getSelectedCandidatePair is {local, remote}
//     RTCIceCandidates out of getLocalCandidates / getRemoteCandidates;
//   - a fingerprint that does not match fails the handshake: state "failed" with an error event first, an RTCError with
//     errorDetail "fingerprint-failure";
//   - the interfaces are exposed as globals and cannot be constructed (TypeError).
import { RTCPeerConnection, RTCDtlsTransport, RTCIceTransport, RTCIceCandidate } from "dyna:webrtc";
import { SHA256Hex } from "dyna:crypto";
import { suite } from "../lib/kit.js";
import { LO, trickle, negotiate, waitState, waitEvent, closeAll, delay } from "./rtc_util.js";

const t = suite("webrtc_dtls_transport");

function track(kind) {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver(kind).receiver.track;
    pc.close();
    return tr;
}
function attr(sdp, name) { const m = new RegExp("\\r\\na=" + name + ":([^\\r]+)").exec(sdp); return m ? m[1] : null; }

t.test("transport is null until associated, then one shared RTCDtlsTransport for every sender, receiver and sctp", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    const ta = x.addTransceiver(track("audio")), tv = x.addTransceiver(track("video"));
    x.createDataChannel("d");
    a.eq(ta.sender.transport, null);
    a.eq(ta.receiver.transport, null);
    await x.setLocalDescription(await x.createOffer());
    const dt = ta.sender.transport;
    a.ok(dt instanceof RTCDtlsTransport);
    a.ok(dt.iceTransport instanceof RTCIceTransport);
    a.eq(dt.state, "new");
    a.eq(dt.iceTransport.state, "new");
    for (const v of [ta.receiver.transport, tv.sender.transport, tv.receiver.transport]) a.eq(v, dt, "shared under BUNDLE");
    a.eq(dt.iceTransport, dt.iceTransport, "same RTCIceTransport every time");
    await y.setRemoteDescription(x.localDescription);
    a.eq(y.getTransceivers()[0].receiver.transport, y.getTransceivers()[1].sender.transport);
    a.ne(y.getTransceivers()[0].receiver.transport, dt, "each connection has its own");
    await y.setLocalDescription(await y.createAnswer());
    await x.setRemoteDescription(y.localDescription);
    a.eq(x.sctp.transport, dt, "RTCSctpTransport.transport is the same RTCDtlsTransport");
    a.eq(Object.prototype.toString.call(dt), "[object RTCDtlsTransport]");
    a.eq(Object.prototype.toString.call(dt.iceTransport), "[object RTCIceTransport]");
    closeAll(x, y);
});

t.test("state machine and events: dtls new -> connecting -> connected, ice checking -> connected, gathering complete", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    x.addTrack(track("audio"));
    const tr = x.getTransceivers()[0];
    await x.setLocalDescription(await x.createOffer());
    const dt = tr.sender.transport, it = dt.iceTransport;
    const seen = [], ice = [], gat = [];
    dt.onstatechange = () => seen.push(dt.state);
    it.addEventListener("statechange", () => ice.push(it.state));
    const order = [];
    it.ongatheringstatechange = () => { gat.push(it.gatheringState); order.push("t-gather:" + it.gatheringState); };
    it.addEventListener("statechange", () => order.push("t-ice:" + it.state));
    x.addEventListener("icegatheringstatechange", () => order.push("pc-gather:" + x.iceGatheringState));
    x.addEventListener("iceconnectionstatechange", () => order.push("pc-ice:" + x.iceConnectionState));
    let pairs = 0;
    it.onselectedcandidatepairchange = () => pairs++;
    await y.setRemoteDescription(x.localDescription);
    await y.setLocalDescription(await y.createAnswer());
    await x.setRemoteDescription(y.localDescription);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    if (it.gatheringState !== "complete") await waitEvent(it, "gatheringstatechange", 5000, () => it.gatheringState === "complete");
    a.deep(seen, ["connecting", "connected"]);
    a.eq(dt.state, "connected");
    a.eq(ice[0], "checking");
    a.ok(["connected", "completed"].includes(it.state), it.state);
    a.eq(ice[ice.length - 1], it.state, "the last event matches the state");
    a.eq(it.state, x.iceConnectionState, "one transport: the same as the connection's ICE state");
    a.eq(gat[gat.length - 1], "complete");
    a.ok(pairs >= 1, "selectedcandidatepairchange fired");
    for (const kind of ["gather", "ice"]) {
        const tl = order.filter((e) => e.startsWith("t-" + kind)), pl = order.filter((e) => e.startsWith("pc-" + kind));
        a.deep(tl.map((e) => e.split(":")[1]), pl.map((e) => e.split(":")[1]), kind + ": the transport and the connection see the same states");
        for (const e of tl) a.ok(order.indexOf(e) < order.indexOf("pc-" + e.slice(2)), e + " fires on the transport before the connection (webrtc-pc 5.6)");
    }
    a.eq(it.component, "rtp");
    a.eq(it.role, "controlling", "the offerer is controlling (RFC 8445 6.1.1)");
    a.eq(y.getTransceivers()[0].receiver.transport.iceTransport.role, "controlled");
    const n = seen.length, ni = ice.length;
    x.close();
    a.eq(dt.state, "closed");
    a.eq(it.state, "closed");
    y.close();
    await delay(200);
    a.eq(seen.length, n, "close() fires no statechange, also not later");
    a.eq(ice.length, ni);
});

t.test("certificates, parameters and candidates match what each side signalled", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    x.addTrack(track("audio"));
    await negotiate(x, y);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    const dt = x.getTransceivers()[0].sender.transport, it = dt.iceTransport;
    const certs = dt.getRemoteCertificates();
    a.eq(certs.length, 1);
    a.ok(certs[0] instanceof ArrayBuffer);
    a.eq(new Uint8Array(certs[0])[0], 0x30, "DER SEQUENCE");
    const fp = attr(y.localDescription.sdp, "fingerprint").split(" ")[1].replace(/:/g, "").toLowerCase();
    a.eq(SHA256Hex(new Uint8Array(certs[0])).toLowerCase(), fp, "SHA-256 of the DER is the peer's a=fingerprint");
    a.ne(certs[0], dt.getRemoteCertificates()[0], "a new ArrayBuffer each call");
    const lp = it.getLocalParameters(), rp = it.getRemoteParameters();
    a.eq(lp.usernameFragment, attr(x.localDescription.sdp, "ice-ufrag"));
    a.eq(lp.password, attr(x.localDescription.sdp, "ice-pwd"));
    a.eq(rp.usernameFragment, attr(y.localDescription.sdp, "ice-ufrag"));
    a.eq(rp.password, attr(y.localDescription.sdp, "ice-pwd"));
    const lc = it.getLocalCandidates(), rc = it.getRemoteCandidates();
    a.ok(lc.length >= 1 && lc.every((c) => c instanceof RTCIceCandidate));
    a.ok(rc.length >= 1 && rc.every((c) => c instanceof RTCIceCandidate));
    a.eq(lc[0].usernameFragment, lp.usernameFragment);
    const pair = it.getSelectedCandidatePair();
    a.ok(pair.local instanceof RTCIceCandidate && pair.remote instanceof RTCIceCandidate);
    a.ok(lc.some((c) => c.candidate === pair.local.candidate), "the selected local candidate is one of ours");
    a.ok(rc.some((c) => c.address === pair.remote.address && c.port === pair.remote.port), "the selected remote is one of theirs");
    x.close();
    a.eq(it.getSelectedCandidatePair(), null, "no pair once closed");
    a.deep(dt.getRemoteCertificates(), []);
    y.close();
});

t.test("a fingerprint mismatch fails DTLS: error event (fingerprint-failure) then statechange to failed", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    x.addTrack(track("audio"));
    await x.setLocalDescription(await x.createOffer());
    await y.setRemoteDescription(x.localDescription);
    await y.setLocalDescription(await y.createAnswer());
    const bad = y.localDescription.sdp.replace(/(a=fingerprint:sha-256 )([0-9A-F]{2})/g, (_, p, h) => p + (h === "00" ? "11" : "00"));
    const dt = x.getTransceivers()[0].sender.transport;
    const order = [];
    let err = null;
    dt.onerror = (e) => { order.push("error:" + dt.state); err = e.error; };
    dt.addEventListener("statechange", () => order.push(dt.state));
    await x.setRemoteDescription({ type: "answer", sdp: bad });
    await waitEvent(dt, "statechange", 10000, () => dt.state === "failed");
    a.deep(order.slice(-2), ["error:failed", "failed"], "error first, then statechange");
    a.ok(err instanceof DOMException);
    a.eq(err.name, "OperationError");
    a.eq(err.errorDetail, "fingerprint-failure");
    closeAll(x, y);
});

t.test("globals exist and the constructors are illegal", (a) => {
    a.eq(typeof globalThis.RTCDtlsTransport, "function");
    a.eq(typeof globalThis.RTCIceTransport, "function");
    a.eq(globalThis.RTCDtlsTransport, RTCDtlsTransport);
    a.throws(() => new RTCDtlsTransport(), TypeError);
    a.throws(() => new RTCIceTransport(), TypeError);
    a.throws(() => RTCDtlsTransport.prototype.state, TypeError, "a getter on the prototype throws");
});

await t.run();
