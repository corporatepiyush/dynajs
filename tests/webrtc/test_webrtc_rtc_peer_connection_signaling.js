// covers: dyna:webrtc.RTCPeerConnection -- the W3C signaling state machine and JSEP surface: offer/answer/pranswer/
// rollback transitions and their events, perfect-negotiation implicit rollback, InvalidStateError /
// InvalidModificationError / OperationError(sdp-syntax-error) paths, implicit SLD, SDP shape (BUNDLE, setup, mid,
// sctp-port, max-message-size, fingerprint; a bundle-only data m= section is accepted, RFC 9143 6), non-trickle exchange, addIceCandidate validation, negotiationneeded,
// ICE restart with data continuity, getStats shape, the RTCSessionDescription / RTCIceCandidate / RTCCertificate
// classes, generateCertificate, and global === module identity.
// Operations chained before close() never settle (webrtc-pc 4.4.1.2 "chain an operation": the queued operation aborts
// when [[IsClosed]]); calls made after close() reject at once with InvalidStateError; getStats still resolves.
// R1b-9: getConfiguration reports the configured bundlePolicy (default "balanced", the RTCConfiguration default);
// setConfiguration with a different bundlePolicy (an omitted one is "balanced") is an InvalidModificationError and an
// unknown bundlePolicy/rtcpMuxPolicy value is a TypeError (webrtc-pc 4.4.1.6 set the configuration, WebIDL enums).
// SPEC-ADDICE: addIceCandidate converts its RTCIceCandidateInit at call time (WebIDL dictionary: members read once,
// in order candidate, sdpMLineIndex, sdpMid, usernameFragment; a throwing member or a non-object rejects the returned
// promise, never throws), so a later mutation of the init object does not reach the queued operation.
import * as W from "dyna:webrtc";
import { RTCPeerConnection, RTCSessionDescription, RTCIceCandidate, RTCCertificate } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { LO, connectPair, waitEvent, waitState, closeAll, trickle, negotiate, delay } from "./rtc_util.js";

const t = suite("webrtc_signaling");
const ISE = { type: DOMException, code: 11 };

function states(pc) {
    const seen = [];
    pc.addEventListener("signalingstatechange", () => seen.push(pc.signalingState));
    return seen;
}

t.test("globals are the module classes", (a) => {
    for (const k of ["RTCPeerConnection", "RTCDataChannel", "RTCIceCandidate", "RTCSessionDescription", "RTCCertificate"]) {
        a.eq(typeof globalThis[k], "function", k + " is a global");
        a.eq(globalThis[k], W[k], k + " global === module export");
    }
    a.throws(() => new RTCCertificate(), TypeError);
    a.throws(() => RTCPeerConnection(), TypeError);
});

t.test("initial state", (a) => {
    const pc = new RTCPeerConnection(LO);
    a.eq(pc.signalingState, "stable");
    a.eq(pc.iceGatheringState, "new");
    a.eq(pc.iceConnectionState, "new");
    a.eq(pc.connectionState, "new");
    a.eq(pc.localDescription, null);
    a.eq(pc.remoteDescription, null);
    a.eq(pc.canTrickleIceCandidates, null);
    a.eq(pc.sctp, null);
    a.deep(pc.getTransceivers(), []);
    const cfg = pc.getConfiguration();
    a.eq(cfg.bundlePolicy, "balanced");
    a.eq(cfg.rtcpMuxPolicy, "require");
    a.eq(Object.prototype.toString.call(pc), "[object RTCPeerConnection]");
    pc.close();
    a.eq(pc.signalingState, "closed");
    a.eq(pc.connectionState, "closed");
});

t.test("operations queued before close() never settle; calls after close() reject", async (a) => {
    const pc = new RTCPeerConnection(LO);
    pc.createDataChannel("x");
    let settled = [];
    pc.createOffer().then(() => settled.push("offer ok"), (e) => settled.push("offer " + e.name));
    pc.setRemoteDescription({ type: "answer", sdp: "x" }).then(() => settled.push("srd ok"), (e) => settled.push("srd " + e.name));
    pc.close();
    await delay(100);
    a.eq(settled.join(","), "", "queued operations never settle");
    let err = null;
    try { await pc.createOffer(); } catch (e) { err = e; }
    a.eq(err && err.name, "InvalidStateError");
    const st = await pc.getStats();
    a.ok(st.size > 0, "getStats resolves after close");
});

t.test("offer SDP shape: BUNDLE, actpass, mid, sctp-port, max-message-size, sha-256 fingerprint, trickle", async (a) => {
    const pc = new RTCPeerConnection(LO);
    pc.createDataChannel("x");
    const o = await pc.createOffer();
    a.eq(o.type, "offer");
    a.match(o.sdp, /^v=0\r\n/);
    a.match(o.sdp, /\r\na=group:BUNDLE 0\r\n/);
    a.match(o.sdp, /\r\nm=application 9 UDP\/DTLS\/SCTP webrtc-datachannel\r\n/);
    a.match(o.sdp, /\r\na=setup:actpass\r\n/);
    a.match(o.sdp, /\r\na=mid:0\r\n/);
    a.match(o.sdp, /\r\na=sctp-port:5000\r\n/);
    a.match(o.sdp, /\r\na=max-message-size:\d+\r\n/);
    a.match(o.sdp, /\r\na=fingerprint:sha-256 ([0-9A-F]{2}:){31}[0-9A-F]{2}\r\n/);
    a.match(o.sdp, /\r\na=ice-options:trickle\r\n/);
    const uf = /a=ice-ufrag:(\S+)/.exec(o.sdp)[1], pw = /a=ice-pwd:(\S+)/.exec(o.sdp)[1];
    a.ok(uf.length >= 4, "ufrag >= 4 chars (RFC 8445 5.3)");
    a.ok(pw.length >= 22, "pwd >= 22 chars (RFC 8445 5.3)");
    const o2 = await pc.createOffer();
    a.eq(/a=ice-ufrag:(\S+)/.exec(o2.sdp)[1], uf, "credentials stable across createOffer without iceRestart");
    pc.close();
});

t.test("empty offer when there is nothing to negotiate", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const o = await pc.createOffer();
    a.ok(!/m=/.test(o.sdp));
    pc.close();
});

t.test("offer/answer transitions and events, current/pending descriptions", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const sx = states(x), sy = states(y);
    x.createDataChannel("d");
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    a.eq(x.signalingState, "have-local-offer");
    a.eq(x.pendingLocalDescription.type, "offer");
    a.eq(x.currentLocalDescription, null);
    await y.setRemoteDescription(o);
    a.eq(y.signalingState, "have-remote-offer");
    a.eq(y.pendingRemoteDescription.sdp, o.sdp);
    a.eq(y.canTrickleIceCandidates, true);
    await a.rejects(y.createOffer().then(() => y.setLocalDescription({ type: "offer", sdp: "v=0\r\n" })), { type: DOMException });
    const ans = await y.createAnswer();
    a.match(ans.sdp, /a=setup:active/);
    await y.setLocalDescription(ans);
    await x.setRemoteDescription(ans);
    a.deep(sx, ["have-local-offer", "stable"]);
    a.deep(sy, ["have-remote-offer", "stable"]);
    a.eq(x.currentRemoteDescription.type, "answer");
    a.eq(x.pendingLocalDescription, null);
    a.eq(x.currentLocalDescription.type, "offer");
    a.ok(x.localDescription instanceof RTCSessionDescription);
    closeAll(x, y);
});

t.test("rollback: local offer and remote offer both roll back to stable", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("d");
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    await x.setLocalDescription({ type: "rollback" });
    a.eq(x.signalingState, "stable");
    a.eq(x.localDescription, null);
    await y.setRemoteDescription(o);
    await y.setRemoteDescription({ type: "rollback" });
    a.eq(y.signalingState, "stable");
    a.eq(y.remoteDescription, null);
    await a.rejects(y.setRemoteDescription({ type: "rollback" }), ISE);
    closeAll(x, y);
});

t.test("glare: a remote offer in have-local-offer rolls the local offer back implicitly", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("x");
    y.createDataChannel("y");
    const ox = await x.createOffer(), oy = await y.createOffer();
    await x.setLocalDescription(ox);
    await y.setLocalDescription(oy);
    await x.setRemoteDescription(oy);
    a.eq(x.signalingState, "have-remote-offer");
    const ans = await x.createAnswer();
    await x.setLocalDescription(ans);
    await y.setRemoteDescription(ans);
    a.eq(x.signalingState, "stable");
    a.eq(y.signalingState, "stable");
    closeAll(x, y);
});

t.test("error paths: createAnswer in stable, answer in stable, modified SDP, bad SDP, missing type", async (a) => {
    const x = new RTCPeerConnection(LO);
    x.createDataChannel("d");
    await a.rejects(x.createAnswer(), ISE);
    const o = await x.createOffer();
    await a.rejects(x.setRemoteDescription({ type: "answer", sdp: o.sdp }), ISE);
    await a.rejects(x.setLocalDescription({ type: "offer", sdp: o.sdp.replace("a=mid:0", "a=mid:1") }), { type: DOMException, message: /does not match/ });
    const y = new RTCPeerConnection(LO);
    let err = null;
    try { await y.setRemoteDescription({ type: "offer", sdp: "garbage" }); } catch (e) { err = e; }
    a.ok(err && err.name === "OperationError", "malformed SDP rejects with OperationError");
    a.eq(err && err.errorDetail, "sdp-syntax-error");
    await a.rejects(y.setRemoteDescription({ sdp: o.sdp }), TypeError);
    await a.rejects(y.setRemoteDescription({ type: "offer", sdp: o.sdp.replace(/a=fingerprint:.*\r\n/, "") }), { type: DOMException, message: /fingerprint/ });
    await a.rejects(y.setRemoteDescription({ type: "offer", sdp: o.sdp.replace(/a=ice-pwd:.*\r\n/, "") }), { type: DOMException, message: /ice-pwd/ });
    a.eq(y.signalingState, "stable", "a failed SRD leaves the state untouched");
    await a.rejects(y.addIceCandidate({ candidate: "candidate:1 1 udp 1 127.0.0.1 9 typ host", sdpMid: "0" }), ISE);
    closeAll(x, y);
});

t.test("implicit setLocalDescription() creates and applies the offer, then the answer", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    x.createDataChannel("i");
    const rp = waitEvent(y, "datachannel");
    await x.setLocalDescription();
    a.eq(x.localDescription.type, "offer");
    await y.setRemoteDescription(x.localDescription);
    await y.setLocalDescription();
    a.eq(y.localDescription.type, "answer");
    await x.setRemoteDescription(y.localDescription);
    await rp;
    closeAll(x, y);
}, { timeoutMs: 15000 });

t.test("a bundle-only data m= section (port 0, RFC 9143 6) in a max-bundle offer is accepted, not rejected", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("b");
    const o = await x.createOffer();
    const cut = o.sdp.indexOf("m=application");
    const tx = o.sdp.slice(cut).split("\r\n").filter((l) => /^a=(ice-ufrag|ice-pwd|fingerprint|setup):/.test(l)).join("\r\n");
    const audio = "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\n" + tx + "\r\na=mid:au\r\na=rtpmap:111 opus/48000/2\r\na=rtcp-mux\r\na=sendrecv\r\n";
    const head = o.sdp.slice(0, cut).replace(/a=group:BUNDLE ([^\r\n]*)/, "a=group:BUNDLE au $1");
    const data = o.sdp.slice(cut).replace(/^m=application 9 /, "m=application 0 ").replace(/\r\na=mid:/, "\r\na=bundle-only\r\na=mid:");
    const sdp = head + audio + data;
    a.match(sdp, /\r\nm=application 0 UDP\/DTLS\/SCTP webrtc-datachannel\r\n/);
    a.match(sdp, /\r\na=bundle-only\r\n/);
    await y.setRemoteDescription({ type: "offer", sdp });
    const ans = await y.createAnswer();
    a.match(ans.sdp, /\r\nm=application 9 UDP\/DTLS\/SCTP webrtc-datachannel\r\n/, "the data m= section is answered with a port, so it is not rejected");
    a.match(ans.sdp, /\r\nm=audio 9 /, "the BUNDLE-tagged audio m= section carries the transport and is accepted");
    a.eq(y.getTransceivers().length, 1, "the audio m= section creates one recvonly transceiver");
    a.match(ans.sdp, /\r\na=sctp-port:\d+\r\n/);
    closeAll(x, y);
}, { timeoutMs: 15000 });

t.test("non-trickle: wait for gathering complete, the descriptions carry the candidates and end-of-candidates", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const dc = x.createDataChannel("nt");
    await x.setLocalDescription(await x.createOffer());
    if (x.iceGatheringState !== "complete") await waitEvent(x, "icegatheringstatechange", 5000, () => x.iceGatheringState === "complete");
    const offer = x.localDescription;
    a.match(offer.sdp, /\r\na=candidate:\S+ 1 udp \d+ 127\.0\.0\.1 \d+ typ host/);
    a.match(offer.sdp, /\r\na=end-of-candidates\r\n/);
    const rp = waitEvent(y, "datachannel");
    await y.setRemoteDescription(offer);
    await y.setLocalDescription(await y.createAnswer());
    if (y.iceGatheringState !== "complete") await waitEvent(y, "icegatheringstatechange", 5000, () => y.iceGatheringState === "complete");
    await x.setRemoteDescription(y.localDescription);
    const ch = (await rp).channel;
    if (dc.readyState !== "open") await waitEvent(dc, "open");
    const m = waitEvent(ch, "message");
    dc.send("nt");
    a.eq((await m).data, "nt");
    closeAll(x, y);
}, { timeoutMs: 15000 });

t.test("events: icecandidate carries RTCIceCandidate then null; gathering new->gathering->complete", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const g = [], c = [];
    pc.onicegatheringstatechange = () => g.push(pc.iceGatheringState);
    const done = new Promise((r) => { pc.onicecandidate = (e) => { c.push(e.candidate); if (!e.candidate) r(); }; });
    pc.createDataChannel("e");
    await pc.setLocalDescription();
    await done;
    a.deep(g, ["gathering", "complete"]);
    a.ok(c[0] instanceof RTCIceCandidate);
    a.eq(c[0].sdpMid, "0");
    a.eq(c[0].sdpMLineIndex, 0);
    a.eq(c[0].type, "host");
    a.eq(c[0].protocol, "udp");
    a.eq(c[0].address, "127.0.0.1");
    a.eq(c[0].component, "rtp");
    a.eq(c[c.length - 1], null);
    pc.close();
}, { timeoutMs: 10000 });

t.test("negotiationneeded fires once for the first data channel, not for the second", async (a) => {
    const pc = new RTCPeerConnection(LO);
    let n = 0;
    pc.onnegotiationneeded = () => n++;
    pc.createDataChannel("a");
    pc.createDataChannel("b");
    await delay(20);
    a.eq(n, 1);
    pc.close();
});

t.test("addIceCandidate validation: unknown mid, wrong ufrag, unparsable, mdns ignored, end-of-candidates", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("v");
    const o = await x.createOffer();
    await y.setRemoteDescription(o);
    const cand = "candidate:1 1 udp 2130706431 127.0.0.1 9 typ host";
    await a.rejects(y.addIceCandidate({ candidate: cand, sdpMid: "nope" }), { type: DOMException, message: /unknown sdpMid/ });
    await a.rejects(y.addIceCandidate({ candidate: cand, sdpMid: "0", usernameFragment: "zzzz" }), { type: DOMException, message: /usernameFragment/ });
    await a.rejects(y.addIceCandidate({ candidate: "candidate:x", sdpMid: "0" }), { type: DOMException, message: /parsed/ });
    await a.rejects(y.addIceCandidate({ candidate: cand }), TypeError);
    await y.addIceCandidate({ candidate: "candidate:1 1 udp 2130706431 abc.local 9 typ host", sdpMid: "0" });
    await y.addIceCandidate({ candidate: cand, sdpMLineIndex: 0 });
    await y.addIceCandidate({ candidate: "", sdpMid: "0" });
    await y.addIceCandidate(null);
    await y.addIceCandidate();
    a.ok(true);
    closeAll(x, y);
});

t.test("addIceCandidate converts the RTCIceCandidateInit at call time (WebIDL)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.createDataChannel("v");
    const o = await x.createOffer();
    const blocker = y.setRemoteDescription(o);
    const log = [];
    const spy = { get candidate() { log.push("candidate"); return ""; }, get sdpMid() { log.push("sdpMid"); return "0"; },
        get sdpMLineIndex() { log.push("sdpMLineIndex"); return null; }, get usernameFragment() { log.push("usernameFragment"); return undefined; } };
    const p1 = y.addIceCandidate(spy);
    a.eq(log.join(","), "candidate,sdpMLineIndex,sdpMid,usernameFragment", "members are read synchronously, once, in WebIDL order");
    const init = { candidate: "", sdpMid: "0" };
    const p2 = y.addIceCandidate(init);
    init.candidate = "candidate:x";
    init.sdpMid = "nope";
    const SENT = new Error("member sentinel");
    let threw = null, p3, p4, p5, p6;
    try {
        p3 = y.addIceCandidate({ get candidate() { throw SENT; } });
        p4 = y.addIceCandidate({ candidate: "c", sdpMid: { toString() { throw SENT; } } });
        p5 = y.addIceCandidate("candidate:1 1 udp 1 127.0.0.1 9 typ host");
        p6 = y.addIceCandidate({ candidate: "candidate:1 1 udp 2130706431 127.0.0.1 9 typ host" });
    } catch (e) { threw = e; }
    a.eq(threw, null, "conversion errors reject the promise instead of throwing");
    let after = "";
    p6.catch(() => { after += "6"; });
    await blocker;
    after += "b";
    await p1;
    await p2;
    a.ok(after.startsWith("6"), "both-null TypeError rejects before the queued setRemoteDescription settles: " + after);
    await a.rejects(p3, /member sentinel/);
    await a.rejects(p4, /member sentinel/);
    await a.rejects(p5, TypeError);
    await a.rejects(p6, TypeError);
    const q = { candidate: "candidate:1 1 udp 2130706431 127.0.0.1 9 typ host", sdpMLineIndex: 65536 };
    await y.addIceCandidate(q);
    closeAll(x, y);
});

t.test("ICE restart: restartIce -> negotiationneeded -> new credentials, the channel keeps working", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    const uf0 = /a=ice-ufrag:(\S+)/.exec(x.localDescription.sdp)[1];
    const yuf0 = /a=ice-ufrag:(\S+)/.exec(y.localDescription.sdp)[1];
    const nn = waitEvent(x, "negotiationneeded");
    x.restartIce();
    await nn;
    await negotiate(x, y);
    const uf1 = /a=ice-ufrag:(\S+)/.exec(x.localDescription.sdp)[1];
    a.ne(uf1, uf0, "new ufrag after restart");
    a.ne(/a=ice-ufrag:(\S+)/.exec(y.localDescription.sdp)[1], yuf0, "the answerer also restarts (RFC 8445 9)");
    await delay(300);
    a.eq(x.connectionState, "connected");
    const m = waitEvent(rdc, "message");
    dc.send("after restart");
    a.eq((await m).data, "after restart");
    const o2 = await x.createOffer({ iceRestart: true });
    a.ne(/a=ice-ufrag:(\S+)/.exec(o2.sdp)[1], uf1, "createOffer({iceRestart:true}) changes credentials");
    closeAll(x, y);
}, { timeoutMs: 20000 });

t.test("getStats: peer-connection, data-channel, transport, candidate-pair, candidates, certificates", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    const m = waitEvent(rdc, "message");
    dc.send("12345");
    await m;
    const s = await x.getStats();
    a.ok(s instanceof Map);
    const by = (type) => [...s.values()].filter((v) => v.type === type);
    a.eq(by("peer-connection").length, 1);
    a.eq(by("peer-connection")[0].dataChannelsOpened, 1);
    const d = by("data-channel")[0];
    a.eq(d.label, "t");
    a.eq(d.state, "open");
    a.eq(d.messagesSent, 1);
    a.eq(d.bytesSent, 5);
    const tr = by("transport")[0];
    a.eq(tr.dtlsState, "connected");
    a.eq(tr.iceRole, "controlling");
    a.eq(tr.dtlsRole, "server");
    a.ok(typeof tr.dtlsCipher === "string" && tr.dtlsCipher.length > 0);
    const sel = s.get(tr.selectedCandidatePairId);
    a.ok(sel && sel.nominated && sel.state === "succeeded");
    a.ok(s.has(sel.localCandidateId) && s.has(sel.remoteCandidateId));
    a.eq(by("certificate").length, 2);
    for (const v of s.values()) a.ok(typeof v.id === "string" && typeof v.timestamp === "number");
    closeAll(x, y);
    const after = await x.getStats();
    a.ok(after instanceof Map, "getStats after close still resolves");
}, { timeoutMs: 15000 });

t.test("RTCSessionDescription and RTCIceCandidate constructors and toJSON", (a) => {
    const d = new RTCSessionDescription({ type: "offer", sdp: "v=0\r\n" });
    a.eq(d.type, "offer");
    a.deep(JSON.parse(JSON.stringify(d)), { type: "offer", sdp: "v=0\r\n" });
    a.eq(new RTCSessionDescription({ type: "rollback" }).sdp, "");
    a.throws(() => new RTCSessionDescription({}), TypeError);
    a.throws(() => new RTCSessionDescription({ type: "bogus" }), TypeError);
    const c = new RTCIceCandidate({ candidate: "candidate:842163049 1 udp 1677729535 203.0.113.7 46154 typ srflx raddr 10.0.0.2 rport 46154 generation 0", sdpMid: "0", sdpMLineIndex: 0, usernameFragment: "abcd" });
    a.eq(c.foundation, "842163049");
    a.eq(c.priority, 1677729535);
    a.eq(c.address, "203.0.113.7");
    a.eq(c.port, 46154);
    a.eq(c.type, "srflx");
    a.eq(c.relatedAddress, "10.0.0.2");
    a.eq(c.relatedPort, 46154);
    a.eq(c.usernameFragment, "abcd");
    a.deep(c.toJSON(), { candidate: c.candidate, sdpMid: "0", sdpMLineIndex: 0, usernameFragment: "abcd" });
    a.throws(() => new RTCIceCandidate({ candidate: "x" }), TypeError);
    const e = new RTCIceCandidate({ sdpMid: "0" });
    a.eq(e.candidate, "");
    a.eq(e.foundation, null);
});

t.test("generateCertificate: ECDSA P-256 and RSA, used in the configuration, fingerprint lands in the SDP", async (a) => {
    const ec = await RTCPeerConnection.generateCertificate({ name: "ECDSA", namedCurve: "P-256" });
    a.ok(ec instanceof RTCCertificate);
    a.ok(ec.expires > Date.now());
    const fp = ec.getFingerprints();
    a.eq(fp[0].algorithm, "sha-256");
    a.match(fp[0].value, /^([0-9A-F]{2}:){31}[0-9A-F]{2}$/);
    const rsa = await RTCPeerConnection.generateCertificate({ name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" });
    a.ok(rsa instanceof RTCCertificate);
    await a.rejects(RTCPeerConnection.generateCertificate({ name: "ECDSA", namedCurve: "P-521" }), { type: DOMException, message: /unsupported/ });
    await a.rejects(RTCPeerConnection.generateCertificate("bogus"), { type: DOMException });
    const pc = new RTCPeerConnection(Object.assign({ certificates: [ec] }, LO));
    pc.createDataChannel("c");
    const o = await pc.createOffer();
    a.ok(o.sdp.includes("a=fingerprint:sha-256 " + fp[0].value));
    a.eq(pc.getConfiguration().certificates[0], ec);
    a.throws(() => pc.setConfiguration({ certificates: [rsa] }), { type: DOMException, message: /certificates/ });
    pc.close();
    const { a: x, b: y } = await connectPair({ configA: Object.assign({ certificates: [rsa] }, LO) });
    a.eq(x.connectionState, "connected", "an RSA certificate completes a DTLS handshake");
    closeAll(x, y);
}, { timeoutMs: 30000 });

t.test("configuration validation", (a) => {
    a.throws(() => new RTCPeerConnection({ iceServers: [{ urls: "http://x" }] }), { type: DOMException, message: /invalid ICE server URL/ });
    a.throws(() => new RTCPeerConnection({ iceServers: [{ urls: "turn:127.0.0.1" }] }), { type: DOMException, message: /username/ });
    a.throws(() => new RTCPeerConnection({ iceServers: [{ urls: "turns:127.0.0.1" , username: "u", credential: "p" }] }), { type: DOMException, message: /not supported/ });
    a.throws(() => new RTCPeerConnection({ iceServers: [{ urls: [] }] }), { type: DOMException, code: 12 });
    a.throws(() => new RTCPeerConnection({ iceTransportPolicy: "bogus" }), TypeError);
    a.throws(() => new RTCPeerConnection({ bundlePolicy: "bogus" }), TypeError);
    a.throws(() => new RTCPeerConnection({ rtcpMuxPolicy: "negotiate" }), TypeError);
    a.throws(() => new RTCPeerConnection({ iceLocalAddresses: ["not-an-ip"] }), TypeError);
    const ok = new RTCPeerConnection({ iceServers: [{ urls: ["stun:127.0.0.1:3478", "turn:[::1]:3478?transport=udp"], username: "u", credential: "p" }] });
    a.eq(ok.getConfiguration().iceServers.length, 1);
    ok.close();
});
t.test("setConfiguration: bundlePolicy cannot change (InvalidModificationError), bad enum values are TypeErrors (R1b-9)", async (a) => {
    const pc = new RTCPeerConnection({ ...LO, bundlePolicy: "max-bundle" });
    a.eq(pc.getConfiguration().bundlePolicy, "max-bundle");
    a.throws(() => pc.setConfiguration({ ...LO, bundlePolicy: "balanced" }), { type: DOMException, code: 13 });
    a.throws(() => pc.setConfiguration({ ...LO }), { type: DOMException, code: 13 });
    a.throws(() => pc.setConfiguration({ ...LO, bundlePolicy: "bogus" }), { type: TypeError });
    a.throws(() => pc.setConfiguration({ ...LO, bundlePolicy: "max-bundle", rtcpMuxPolicy: "negotiate" }), { type: TypeError });
    pc.setConfiguration({ ...LO, bundlePolicy: "max-bundle", rtcpMuxPolicy: "require" });
    a.eq(pc.getConfiguration().bundlePolicy, "max-bundle");
    pc.close();
    const d = new RTCPeerConnection(LO);
    a.eq(d.getConfiguration().bundlePolicy, "balanced", "the RTCConfiguration default");
    d.setConfiguration({ ...LO });
    d.close();
});


await t.run();
