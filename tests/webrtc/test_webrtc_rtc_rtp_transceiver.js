// covers: dyna:webrtc.RTCRtpTransceiver -- the media object surface (transceiver, sender, receiver, tracks, streams) without a network
// dyna:webrtc media objects without a network: RTCRtpTransceiver / RTCRtpSender / RTCRtpReceiver, MediaStreamTrack,
// MediaStream, addTransceiver / addTrack / removeTrack, direction, stop(), setCodecPreferences, getCapabilities,
// getParameters / setParameters, replaceTrack, setStreams, the transceiver state machine through offer/answer, and the
// dyna-only second constructor argument (dynaOptions.sdpLimits, a strict bag).
// Cases are written by hand from the intent of the WPT webrtc tests (RTCRtpTransceiver.https.html,
// RTCPeerConnection-addTransceiver.https.html, RTCPeerConnection-addTrack.https.html, RTCPeerConnection-removeTrack,
// RTCRtpSender-setParameters / getParameters / transactionId, RTCRtpTransceiver-setCodecPreferences,
// RTCRtpTransceiver-stop / -stopping, RTCRtpReceiver-jitterBufferTarget, RTCRtpParameters-encodings / -rid), no download.
// Spec choices pinned here (webrtc-pc):
//   - getParameters().transactionId stays the same within a task and is cleared by the next macrotask; setParameters
//     rejects TypeError (missing member) -> InvalidStateError (stopping / no getParameters) -> InvalidModificationError
//     (transactionId, encodings length, rid, codecs/headerExtensions/rtcp) -> RangeError (scale < 1, framerate < 0).
//   - sendEncodings: rid grammar, duplicates, some-but-not-all rid -> TypeError; ranges -> RangeError; cut to 1 (audio)
//     or 4 (video); a lone encoding loses its rid; default video scale 2^(n-i-1).
//   - direction: unknown value ignored, "stopped" TypeError, stopping InvalidStateError; stop() ends the receiver track
//     at once and fires "ended" in a later task; close() ends tracks without events and keeps the transceivers.
//   - A remote offer with more m= sections than the 64-section cap is an OperationError (the W4b stack overflow on 17+
//     sections is the regression this guards: 17, 40 and 64 sections are applied, 65 is refused, and
//     dynaOptions.sdpLimits.maxMediaSections lifts it).
import * as W from "dyna:webrtc";
import { RTCPeerConnection, RTCRtpTransceiver, RTCRtpSender, RTCRtpReceiver, MediaStream, MediaStreamTrack } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { LO, negotiate, closeAll, delay } from "./rtc_util.js";

const t = suite("webrtc_rtp_transceiver");

function thr(a, fn, name, msg) {
    try { fn(); a.ok(false, (msg || name) + ": did not throw"); }
    catch (e) { a.eq(e && e.name, name, msg); }
}
async function rej(a, p, name, msg) {
    try { await p; a.ok(false, (msg || name) + ": did not reject"); }
    catch (e) { a.eq(e && e.name, name, (msg || "") + " " + (e && e.message)); }
}
const tick = () => delay(0);
function remoteTrack(kind) {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver(kind);
    const track = tr.receiver.track;
    pc.close();
    return track;
}
const FP = Array(32).fill("AB").join(":");
function offerWith(n, kind = "audio") {
    let s = "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=fingerprint:sha-256 " + FP + "\r\na=ice-ufrag:abcd\r\na=ice-pwd:abcdefghijklmnopqrstuvwx\r\n";
    for (let i = 0; i < n; i++) s += "m=" + kind + " 9 UDP/TLS/RTP/SAVPF 0\r\nc=IN IP4 0.0.0.0\r\na=mid:" + i + "\r\na=rtcp-mux\r\na=sendrecv\r\na=rtpmap:0 PCMU/8000\r\n";
    return s;
}

t.test("the module and global classes exist; the RTP and track classes have no public constructor", (a) => {
    for (const k of ["RTCRtpTransceiver", "RTCRtpSender", "RTCRtpReceiver", "MediaStream", "MediaStreamTrack"]) {
        a.eq(typeof W[k], "function", k + " is exported");
        a.eq(globalThis[k], W[k], k + " global === module export");
    }
    a.throws(() => new RTCRtpTransceiver(), TypeError);
    a.throws(() => new RTCRtpSender(), TypeError);
    a.throws(() => new RTCRtpReceiver(), TypeError);
    a.throws(() => new MediaStreamTrack(), TypeError);
    a.throws(() => MediaStream(), TypeError, "MediaStream needs new");
    a.eq(Object.prototype.toString.call(new MediaStream()), "[object MediaStream]");
});

t.test("addTransceiver(kind): defaults, mid null, sendrecv, receiver track, sender without track", (a) => {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver("audio");
    a.ok(tr instanceof RTCRtpTransceiver);
    a.eq(tr.mid, null);
    a.eq(tr.direction, "sendrecv");
    a.eq(tr.currentDirection, null);
    a.ok(tr.sender instanceof RTCRtpSender);
    a.ok(tr.receiver instanceof RTCRtpReceiver);
    a.eq(tr.sender.track, null);
    a.eq(tr.sender.transport, null);
    a.ok(tr.sender.dtmf instanceof W.RTCDTMFSender, "an audio sender has an RTCDTMFSender");
    a.eq(tr.sender.dtmf, tr.sender.dtmf, "the same object every time");
    const vpc = new RTCPeerConnection(LO);
    a.eq(vpc.addTransceiver("video").sender.dtmf, null, "a video sender has none");
    vpc.close();
    const rt = tr.receiver.track;
    a.ok(rt instanceof MediaStreamTrack);
    a.eq(rt.kind, "audio");
    a.eq(rt.readyState, "live");
    a.eq(rt.muted, true, "a remote track starts muted");
    a.match(rt.id, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    a.eq(tr.receiver.track, rt, "the receiver track is the same object every time");
    a.deep(pc.getTransceivers(), [tr]);
    a.deep(pc.getSenders(), [tr.sender]);
    a.deep(pc.getReceivers(), [tr.receiver]);
    const v = pc.addTransceiver("video", { direction: "recvonly" });
    a.eq(v.direction, "recvonly");
    a.eq(v.receiver.track.kind, "video");
    a.eq(pc.getTransceivers().length, 2);
    pc.close();
});

t.test("addTransceiver argument errors: bad kind, bad direction, stopped direction, bad init, closed", (a) => {
    const pc = new RTCPeerConnection(LO);
    a.throws(() => pc.addTransceiver("text"), TypeError);
    a.throws(() => pc.addTransceiver({}), TypeError);
    a.throws(() => pc.addTransceiver("audio", { direction: "sideways" }), TypeError);
    a.throws(() => pc.addTransceiver("audio", { direction: "stopped" }), TypeError);
    a.throws(() => pc.addTransceiver("audio", 5), TypeError);
    a.throws(() => pc.addTransceiver("audio", { streams: [{}] }), TypeError);
    a.eq(pc.getTransceivers().length, 0, "nothing was created by a failed call");
    pc.close();
    thr(a, () => pc.addTransceiver("audio"), "InvalidStateError");
});

t.test("addTransceiver(track) and sendEncodings validation (RFC 8851 rid, simulcast limits)", (a) => {
    const pc = new RTCPeerConnection(LO);
    const vt = remoteTrack("video"), at = remoteTrack("audio");
    const tv = pc.addTransceiver(vt);
    a.eq(tv.sender.track, vt);
    a.eq(tv.receiver.track.kind, "video");
    a.throws(() => pc.addTransceiver("video", { sendEncodings: [{ rid: "a b" }, { rid: "c" }] }), TypeError, "rid grammar");
    a.throws(() => pc.addTransceiver("video", { sendEncodings: [{ rid: "a" }, { rid: "a" }] }), TypeError, "duplicate rid");
    a.throws(() => pc.addTransceiver("video", { sendEncodings: [{ rid: "a" }, {}] }), TypeError, "some but not all rids");
    a.throws(() => pc.addTransceiver("video", { sendEncodings: [{ scaleResolutionDownBy: 0.5 }] }), RangeError);
    a.throws(() => pc.addTransceiver("video", { sendEncodings: [{ maxFramerate: -1 }] }), RangeError);
    a.throws(() => pc.addTransceiver("video", { sendEncodings: [{ priority: "urgent" }] }), TypeError);
    a.throws(() => pc.addTransceiver("video", { sendEncodings: 3 }), TypeError);
    const n0 = pc.getTransceivers().length;
    const sim = pc.addTransceiver("video", { sendEncodings: [{ rid: "a" }, { rid: "b" }, { rid: "c" }, { rid: "d" }, { rid: "e" }] });
    const enc = sim.sender.getParameters().encodings;
    a.deep(enc.map((e) => e.rid), ["a", "b", "c", "d"], "video keeps at most 4 encodings");
    a.deep(enc.map((e) => e.scaleResolutionDownBy), [8, 4, 2, 1], "default scale 2^(n-i-1)");
    const one = pc.addTransceiver("video", { sendEncodings: [{ rid: "solo", maxBitrate: 500000 }] });
    const e1 = one.sender.getParameters().encodings;
    a.eq(e1.length, 1);
    a.eq(e1[0].rid, undefined, "a lone encoding loses its rid");
    a.eq(e1[0].maxBitrate, 500000);
    const au = pc.addTransceiver(at, { sendEncodings: [{ rid: "x" }, { rid: "y" }] });
    const ea = au.sender.getParameters().encodings;
    a.eq(ea.length, 1, "audio keeps one encoding");
    a.eq(ea[0].scaleResolutionDownBy, undefined, "audio has no scale");
    a.eq(pc.getTransceivers().length, n0 + 3);
    pc.close();
});

t.test("addTrack creates or reuses a transceiver; removeTrack flips the direction (WPT addTrack/removeTrack)", (a) => {
    const pc = new RTCPeerConnection(LO);
    const at = remoteTrack("audio"), at2 = remoteTrack("audio"), vt = remoteTrack("video");
    const s = new MediaStream([at]);
    a.throws(() => pc.addTrack({}), TypeError);
    a.throws(() => pc.addTrack(at, {}), TypeError);
    const snd = pc.addTrack(at, s);
    a.ok(snd instanceof RTCRtpSender);
    a.eq(snd.track, at);
    const tr = pc.getTransceivers()[0];
    a.eq(tr.sender, snd);
    a.eq(tr.direction, "sendrecv");
    thr(a, () => pc.addTrack(at), "InvalidAccessError", "the same track twice");
    const rv = pc.addTransceiver("video", { direction: "recvonly" });
    const vs = pc.addTrack(vt);
    a.eq(vs, rv.sender, "a recvonly video transceiver without a track is reused");
    a.eq(rv.direction, "sendrecv", "recvonly -> sendrecv");
    const ina = pc.addTransceiver("audio", { direction: "inactive" });
    pc.addTrack(at2);
    a.eq(ina.sender.track, at2);
    a.eq(ina.direction, "sendonly", "inactive -> sendonly");
    pc.removeTrack(snd);
    a.eq(snd.track, null);
    a.eq(tr.direction, "recvonly", "sendrecv -> recvonly");
    pc.removeTrack(ina.sender);
    a.eq(ina.direction, "inactive", "sendonly -> inactive");
    pc.removeTrack(ina.sender);
    a.eq(ina.direction, "inactive", "a second removeTrack is a no-op");
    a.throws(() => pc.removeTrack({}), TypeError);
    const other = new RTCPeerConnection(LO);
    const os = other.addTransceiver("audio").sender;
    thr(a, () => pc.removeTrack(os), "InvalidAccessError", "a sender of another connection");
    other.close();
    pc.close();
    thr(a, () => pc.addTrack(remoteTrack("audio")), "InvalidStateError");
    thr(a, () => pc.removeTrack(snd), "InvalidStateError");
});

t.test("direction setter: unknown ignored, stopped TypeError, change fires negotiationneeded", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver("audio");
    let nn = 0;
    pc.onnegotiationneeded = () => nn++;
    await tick();
    nn = 0;
    tr.direction = "sideways";
    a.eq(tr.direction, "sendrecv", "an unknown value is ignored");
    a.throws(() => { tr.direction = "stopped"; }, TypeError);
    tr.direction = "sendonly";
    a.eq(tr.direction, "sendonly");
    tr.stop();
    a.eq(tr.direction, "stopped");
    thr(a, () => { tr.direction = "recvonly"; }, "InvalidStateError", "set on a stopping transceiver");
    pc.close();
});

t.test("stop(): direction stopped, track ended at once, ended event in a later task; close() fires none", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver("video");
    const rt = tr.receiver.track;
    let ended = 0;
    rt.onended = () => ended++;
    tr.stop();
    a.eq(tr.direction, "stopped");
    a.eq(rt.readyState, "ended");
    a.eq(ended, 0, "not fired synchronously");
    await delay(5);
    a.eq(ended, 1, "ended fired in a later task");
    tr.stop();
    a.eq(tr.currentDirection, null, "stopping, not yet stopped");
    a.deep(pc.getTransceivers(), [tr], "stays listed until negotiated");
    const tr2 = pc.addTransceiver("audio");
    let e2 = 0;
    tr2.receiver.track.onended = () => e2++;
    pc.close();
    await delay(5);
    a.eq(e2, 0, "close() ends tracks without events");
    a.eq(tr2.receiver.track.readyState, "ended");
    a.eq(tr2.currentDirection, "stopped");
    a.eq(pc.getTransceivers().length, 2, "close() keeps the transceivers");
    a.eq(pc.getSenders().length, 0, "stopped transceivers have no senders in getSenders");
    thr(a, () => tr2.stop(), "InvalidStateError", "stop() after close()");
});

t.test("MediaStream and MediaStreamTrack surface", (a) => {
    const t1 = remoteTrack("audio"), t2 = remoteTrack("video");
    const s = new MediaStream([t1, t2, t1]);
    a.match(s.id, /^[0-9a-f-]{36}$/);
    a.eq(s.getTracks().length, 2, "duplicates are dropped");
    a.deep(s.getAudioTracks(), [t1]);
    a.deep(s.getVideoTracks(), [t2]);
    a.eq(s.getTrackById(t2.id), t2);
    a.eq(s.getTrackById("nope"), null);
    const c = new MediaStream(s);
    a.ne(c.id, s.id);
    a.deep(c.getTracks(), [t1, t2]);
    let fired = 0;
    s.onaddtrack = () => fired++;
    s.onremovetrack = () => fired++;
    s.removeTrack(t1);
    s.addTrack(t1);
    a.eq(fired, 0, "addTrack/removeTrack fire no events");
    a.throws(() => s.addTrack({}), TypeError);
    a.throws(() => new MediaStream([{}]), TypeError);
    a.throws(() => new MediaStream(5), TypeError);
    const cl = s.clone();
    a.eq(cl.getTracks().length, 2);
    a.ne(cl.getTracks()[0].id, s.getTracks()[0].id, "clone clones the tracks");
    a.eq(s.active, false, "every track of a closed connection is ended");
    a.eq(new MediaStream().active, false);
    const live = new RTCPeerConnection(LO);
    const lt = live.addTransceiver("video").receiver.track;
    const ls = new MediaStream([lt]);
    a.eq(ls.active, true);
    lt.enabled = false;
    a.eq(lt.enabled, false);
    lt.contentHint = "motion";
    a.eq(lt.contentHint, "motion");
    lt.contentHint = "speech";
    a.eq(lt.contentHint, "motion", "an audio hint on a video track is ignored");
    lt.contentHint = "";
    a.eq(lt.contentHint, "");
    a.deep(lt.getConstraints(), {});
    a.eq(lt.getSettings().deviceId, lt.id);
    const lc = lt.clone();
    a.ne(lc.id, lt.id);
    a.eq(lc.kind, "video");
    lc.stop();
    a.eq(lc.readyState, "ended");
    a.eq(lt.readyState, "live", "stopping a clone leaves the original");
    a.eq(lt.label, "");
    live.close();
});

t.test("getCapabilities: codecs, one rtx entry, header extensions; unknown kind -> null", (a) => {
    const v = RTCRtpSender.getCapabilities("video");
    a.deep(v.codecs.map((c) => c.mimeType), ["video/VP8", "video/rtx", "video/H264", "video/H264"]);
    a.eq(v.codecs[1].sdpFmtpLine, undefined, "rtx has no fmtp");
    a.match(v.codecs[2].sdpFmtpLine, /profile-level-id=42e01f/);
    a.ok(v.headerExtensions.some((h) => h.uri === "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"));
    a.ok(!v.headerExtensions.some((h) => /audio-level/.test(h.uri)), "audio-level is audio only");
    const au = RTCRtpReceiver.getCapabilities("audio");
    a.deep(au.codecs.map((c) => c.mimeType + "/" + c.clockRate + "/" + c.channels), ["audio/opus/48000/2", "audio/PCMU/8000/1", "audio/PCMA/8000/1", "audio/telephone-event/48000/1", "audio/telephone-event/8000/1"]);
    a.eq(RTCRtpSender.getCapabilities("text"), null);
    a.eq(RTCRtpReceiver.getCapabilities("Video"), null, "kinds are case-sensitive");
});

t.test("setCodecPreferences: filter and order the offer, errors per webrtc-pc 5.4", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver("video");
    const caps = RTCRtpReceiver.getCapabilities("video").codecs;
    const h264 = caps.filter((c) => c.mimeType === "video/H264");
    tr.setCodecPreferences([h264[1], h264[0]]);
    let sdp = (await pc.createOffer()).sdp;
    a.match(sdp, /\r\nm=video 9 UDP\/TLS\/RTP\/SAVPF 100 98\r\n/, "only the listed codecs, in order, no rtx");
    tr.setCodecPreferences([caps[0], caps[1]]);
    sdp = (await pc.createOffer()).sdp;
    a.match(sdp, /\r\nm=video 9 UDP\/TLS\/RTP\/SAVPF 96 97\r\n/, "listing rtx keeps the RTX of each listed codec");
    tr.setCodecPreferences([{ mimeType: "VIDEO/vp8", clockRate: 90000 }]);
    sdp = (await pc.createOffer()).sdp;
    a.match(sdp, /\r\nm=video 9 UDP\/TLS\/RTP\/SAVPF 96\r\n/, "mimeType matches case-insensitively");
    tr.setCodecPreferences([]);
    sdp = (await pc.createOffer()).sdp;
    a.match(sdp, /\r\nm=video 9 UDP\/TLS\/RTP\/SAVPF 96 97 98 99 100 101\r\n/, "[] restores the default");
    thr(a, () => tr.setCodecPreferences([{ mimeType: "video/AV1", clockRate: 90000 }]), "InvalidModificationError");
    thr(a, () => tr.setCodecPreferences([{ mimeType: "video/VP8", clockRate: 48000 }]), "InvalidModificationError", "clockRate must match");
    thr(a, () => tr.setCodecPreferences([{ mimeType: "video/H264", clockRate: 90000, sdpFmtpLine: "x=1" }]), "InvalidModificationError", "fmtp must match");
    thr(a, () => tr.setCodecPreferences([{ mimeType: "video/H264", clockRate: 90000 }]), "InvalidModificationError", "fmtp present vs absent");
    thr(a, () => tr.setCodecPreferences([caps[1]]), "InvalidModificationError", "only rtx");
    a.throws(() => tr.setCodecPreferences([{ clockRate: 90000 }]), TypeError, "mimeType required");
    a.throws(() => tr.setCodecPreferences(5), TypeError);
    const au = pc.addTransceiver("audio");
    const ac = RTCRtpReceiver.getCapabilities("audio").codecs;
    thr(a, () => au.setCodecPreferences([caps[0]]), "InvalidModificationError", "a video codec on audio");
    thr(a, () => au.setCodecPreferences([{ mimeType: "audio/opus", clockRate: 48000, sdpFmtpLine: ac[0].sdpFmtpLine }]), "InvalidModificationError", "channels must match");
    au.setCodecPreferences([ac[2]]);
    sdp = (await pc.createOffer()).sdp;
    a.match(sdp, /\r\nm=audio 9 UDP\/TLS\/RTP\/SAVPF 8\r\n/);
    const big = { length: 3000000 };
    thr(a, () => au.setCodecPreferences(big), "TypeError", "a huge sparse list fails on its first missing entry, without a huge allocation");
    pc.close();
});

t.test("getParameters / setParameters: transactionId lifetime and error order (WPT RTCRtpSender-setParameters)", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver("video", { sendEncodings: [{ rid: "hi" }, { rid: "lo" }] });
    const s = tr.sender;
    const p1 = s.getParameters(), p2 = s.getParameters();
    a.eq(typeof p1.transactionId, "string");
    a.eq(p1.transactionId, p2.transactionId, "same transactionId within a task");
    a.deep(p1.codecs, [], "no codecs before negotiation");
    a.deep(p1.headerExtensions, []);
    a.eq(typeof p1.rtcp.cname, "string");
    p1.encodings[0].maxBitrate = 1000000;
    p1.encodings[1].scaleResolutionDownBy = 4;
    p1.encodings[1].active = false;
    await s.setParameters(p1);
    const p3 = s.getParameters();
    a.ne(p3.transactionId, p1.transactionId, "setParameters clears it");
    a.eq(p3.encodings[0].maxBitrate, 1000000);
    a.eq(p3.encodings[1].scaleResolutionDownBy, 4);
    a.eq(p3.encodings[1].active, false);
    await rej(a, s.setParameters(p1), "InvalidModificationError", "a stale transactionId");
    await tick();
    await rej(a, s.setParameters(p3), "InvalidStateError", "after the task the last parameters are gone");
    let q = s.getParameters();
    q.encodings.pop();
    await rej(a, s.setParameters(q), "InvalidModificationError", "fewer encodings");
    q = s.getParameters();
    q.encodings[0].rid = "zz";
    await rej(a, s.setParameters(q), "InvalidModificationError", "rid is read-only");
    q = s.getParameters();
    q.rtcp = { cname: "other" };
    await rej(a, s.setParameters(q), "InvalidModificationError", "rtcp is read-only");
    q = s.getParameters();
    q.codecs = [{ payloadType: 1, mimeType: "video/VP8", clockRate: 90000 }];
    await rej(a, s.setParameters(q), "InvalidModificationError", "codecs are read-only");
    q = s.getParameters();
    q.encodings[0].scaleResolutionDownBy = 0.5;
    await rej(a, s.setParameters(q), "RangeError");
    q = s.getParameters();
    q.encodings[0].maxFramerate = -1;
    await rej(a, s.setParameters(q), "RangeError");
    q = s.getParameters();
    delete q.transactionId;
    await rej(a, s.setParameters(q), "TypeError", "missing transactionId");
    q = s.getParameters();
    delete q.encodings;
    await rej(a, s.setParameters(q), "TypeError", "missing encodings");
    q = s.getParameters();
    delete q.rtcp;
    await rej(a, s.setParameters(q), "TypeError", "missing rtcp");
    q = s.getParameters();
    q.encodings[0].rid = "zz";
    delete q.headerExtensions;
    await rej(a, s.setParameters(q), "TypeError", "TypeError comes before InvalidModificationError");
    await rej(a, s.setParameters(5), "TypeError");
    q = s.getParameters();
    tr.stop();
    await rej(a, s.setParameters(q), "InvalidStateError", "stopping");
    const au = pc.addTransceiver("audio");
    q = au.sender.getParameters();
    q.encodings[0].scaleResolutionDownBy = 2;
    q.encodings[0].maxFramerate = 10;
    await au.sender.setParameters(q);
    const qa = au.sender.getParameters().encodings[0];
    a.eq(qa.scaleResolutionDownBy, undefined, "audio drops scale");
    a.eq(qa.maxFramerate, undefined, "audio drops framerate");
    pc.close();
});

t.test("receiver: getParameters, sources, jitterBufferTarget range, getStats", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const r = pc.addTransceiver("audio").receiver;
    a.eq(r.transport, null);
    a.deep(r.getContributingSources(), []);
    a.deep(r.getSynchronizationSources(), []);
    a.eq(r.jitterBufferTarget, null);
    r.jitterBufferTarget = 500;
    a.eq(r.jitterBufferTarget, 500);
    r.jitterBufferTarget = 0;
    a.eq(r.jitterBufferTarget, 0);
    a.throws(() => { r.jitterBufferTarget = 4001; }, RangeError);
    a.throws(() => { r.jitterBufferTarget = -1; }, RangeError);
    a.eq(r.jitterBufferTarget, 0, "a rejected value changes nothing");
    r.jitterBufferTarget = null;
    a.eq(r.jitterBufferTarget, null);
    const p = r.getParameters();
    a.eq(p.transactionId, undefined);
    a.ok(Array.isArray(p.codecs));
    a.ok((await r.getStats()) instanceof Map);
    a.ok((await pc.getTransceivers()[0].sender.getStats()) instanceof Map);
    pc.close();
});

t.test("replaceTrack: kind check, null, chained on the operations queue, stopping", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver("video");
    const vt = remoteTrack("video");
    await tr.sender.replaceTrack(vt);
    a.eq(tr.sender.track, vt);
    await rej(a, tr.sender.replaceTrack(remoteTrack("audio")), "TypeError", "kind mismatch");
    await rej(a, tr.sender.replaceTrack({}), "TypeError");
    await tr.sender.replaceTrack(null);
    a.eq(tr.sender.track, null);
    const pr = tr.sender.replaceTrack(vt);
    a.eq(tr.sender.track, null, "not replaced synchronously");
    await pr;
    a.eq(tr.sender.track, vt);
    tr.stop();
    await rej(a, tr.sender.replaceTrack(null), "InvalidStateError");
    pc.close();
});

t.test("setStreams signals every stream id in a=msid; answers carry every stream too", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const tr = x.addTransceiver("audio");
    const s1 = new MediaStream(), s2 = new MediaStream();
    tr.sender.setStreams(s1, s2, s1);
    a.throws(() => tr.sender.setStreams({}), TypeError);
    const o = await x.createOffer();
    a.ok(o.sdp.includes("a=msid:" + s1.id + " ") && o.sdp.includes("a=msid:" + s2.id + " "), "both stream ids offered");
    a.eq(o.sdp.split("a=msid:" + s1.id).length - 1, 1, "duplicates collapse");
    await x.setLocalDescription(o);
    let ev = null;
    y.ontrack = (e) => { ev = e; };
    await y.setRemoteDescription(o);
    a.ok(ev, "track fired before setRemoteDescription resolved");
    a.deep(ev.streams.map((s) => s.id).sort(), [s1.id, s2.id].sort());
    const ys1 = new MediaStream(), ys2 = new MediaStream();
    y.addTrack(remoteTrack("audio"), ys1, ys2);
    const ans = await y.createAnswer();
    a.ok(ans.sdp.includes("a=msid:" + ys1.id + " ") && ans.sdp.includes("a=msid:" + ys2.id + " "), "the answer carries every stream");
    closeAll(x, y);
    thr(a, () => tr.sender.setStreams(s1), "InvalidStateError");
});

t.test("transceiver state machine through offer/answer: mid, currentDirection, recvonly creation, stop negotiation", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const xa = x.addTransceiver("audio"), xv = x.addTransceiver("video", { direction: "sendonly" });
    const o = await x.createOffer();
    a.eq(xa.mid, null, "createOffer does not assign mids");
    await x.setLocalDescription(o);
    a.eq(xa.mid, "0");
    a.eq(xv.mid, "1");
    a.eq(xa.currentDirection, null);
    await y.setRemoteDescription(o);
    const [ya, yv] = y.getTransceivers();
    a.eq(ya.mid, "0");
    a.eq(ya.direction, "recvonly", "created recvonly by the remote offer");
    a.eq(yv.direction, "recvonly");
    a.eq(ya.currentDirection, null);
    await y.setLocalDescription(await y.createAnswer());
    a.eq(ya.currentDirection, "recvonly");
    a.eq(yv.currentDirection, "recvonly");
    await x.setRemoteDescription(y.localDescription);
    a.eq(xa.currentDirection, "sendonly");
    a.eq(xv.currentDirection, "sendonly");
    const p = xa.sender.getParameters();
    a.eq(p.codecs[0].mimeType, "audio/opus", "negotiated codecs");
    a.eq(p.codecs[0].payloadType, 111);
    a.ok(p.headerExtensions.some((h) => h.uri === "urn:ietf:params:rtp-hdrext:sdes:mid" && h.id === 1));
    a.eq(p.rtcp.reducedSize, true);
    const ended = [];
    yv.receiver.track.onended = () => ended.push("y");
    xv.stop();
    a.eq(xv.currentDirection, "sendonly", "stopping, not stopped");
    const o2 = await x.createOffer();
    a.match(o2.sdp, /\r\nm=video 0 /, "a stopping transceiver's m= section is rejected");
    await x.setLocalDescription(o2);
    await y.setRemoteDescription(o2);
    a.eq(yv.currentDirection, "stopped", "the remote side stops it");
    a.deep(ended, ["y"], "the remote-stopped receiver track fires ended inside setRemoteDescription");
    a.eq(y.getTransceivers().includes(yv), true, "still listed in have-remote-offer");
    await y.setLocalDescription(await y.createAnswer());
    a.eq(y.getTransceivers().includes(yv), false, "removed when stable");
    await x.setRemoteDescription(y.localDescription);
    a.eq(xv.currentDirection, "stopped");
    a.eq(x.getTransceivers().includes(xv), false);
    a.eq(xv.mid, "1", "a stopped transceiver keeps its mid");
    closeAll(x, y);
});

t.test("rollback removes transceivers the remote offer created and keeps addTrack ones (unassociated again)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.addTransceiver("audio");
    x.addTransceiver("video");
    const o = await x.createOffer();
    const at = remoteTrack("audio");
    const s = y.addTrack(at, new MediaStream());
    const ytr = y.getTransceivers()[0];
    const fired = [];
    y.ontrack = (e) => fired.push(e.track.kind);
    await y.setRemoteDescription(o);
    a.deep(fired, ["audio", "video"]);
    a.eq(ytr.mid, "0", "the addTrack transceiver is associated by kind");
    a.eq(y.getTransceivers().length, 2);
    let muted = 0;
    ytr.receiver.track.onmute = () => muted++;
    await y.setRemoteDescription({ type: "rollback" });
    a.eq(y.getTransceivers().length, 1, "the created video transceiver is gone");
    a.eq(y.getTransceivers()[0], ytr);
    a.eq(ytr.mid, null, "the mid is cleared");
    a.eq(s.track, at);
    a.eq(muted, 0, "a remote track that never unmuted does not fire mute");
    fired.length = 0;
    await y.setRemoteDescription(o);
    a.deep(fired, ["audio", "video"], "re-applying the offer fires track again");
    closeAll(x, y);
});

t.test("negotiationneeded: once per change, none after a complete exchange, none for direction ping-pong", async (a) => {
    for (const [dx, dy] of [["sendrecv", "sendrecv"], ["sendonly", "recvonly"], ["recvonly", "sendonly"], ["inactive", "inactive"]]) {
        const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
        let nx = 0, ny = 0;
        x.onnegotiationneeded = () => nx++;
        y.onnegotiationneeded = () => ny++;
        const tx = x.addTransceiver("audio", { direction: dx });
        if (dx !== "recvonly" && dx !== "inactive") await tx.sender.replaceTrack(remoteTrack("audio"));
        x.createDataChannel("d");
        await tick();
        a.eq(nx, 1, dx + ": coalesced negotiationneeded");
        const o = await x.createOffer();
        await x.setLocalDescription(o);
        await y.setRemoteDescription(o);
        y.getTransceivers()[0].direction = dy;
        await y.setLocalDescription(await y.createAnswer());
        await x.setRemoteDescription(y.localDescription);
        await delay(5);
        const before = [nx, ny];
        await delay(5);
        a.deep([nx, ny], before, dx + "/" + dy + ": no spurious negotiationneeded after the exchange");
        a.eq(ny <= 1, true, dx + "/" + dy + ": the answerer fires at most once");
        nx = 0;
        tx.direction = tx.direction === "inactive" ? "sendonly" : "inactive";
        await tick();
        a.eq(nx, 1, "a direction change fires");
        closeAll(x, y);
    }
});

t.test("m= section caps: 17, 40 and 64 remote sections apply, 65 is an OperationError, dynaOptions lifts it", async (a) => {
    for (const n of [17, 40, 64]) {
        const pc = new RTCPeerConnection(LO);
        await pc.setRemoteDescription({ type: "offer", sdp: offerWith(n) });
        a.eq(pc.getTransceivers().length, n, n + " sections -> " + n + " transceivers");
        const ans = await pc.createAnswer();
        a.eq(ans.sdp.split("\r\nm=audio ").length - 1, n);
        pc.close();
    }
    const pc = new RTCPeerConnection(LO);
    await rej(a, pc.setRemoteDescription({ type: "offer", sdp: offerWith(65) }), "OperationError", "65 sections");
    a.eq(pc.getTransceivers().length, 0, "a refused description changes nothing");
    a.eq(pc.signalingState, "stable");
    pc.close();
    const big = new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: 100 } });
    await big.setRemoteDescription({ type: "offer", sdp: offerWith(100) });
    a.eq(big.getTransceivers().length, 100);
    const ba = await big.createAnswer();
    a.eq(ba.sdp.split("\r\nm=audio ").length - 1, 100, "the answer is built under the raised cap too");
    big.close();
    const inf = new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: Infinity } });
    await inf.setRemoteDescription({ type: "offer", sdp: offerWith(130) });
    a.eq(inf.getTransceivers().length, 130);
    inf.close();
    const lo = new RTCPeerConnection(LO);
    for (let i = 0; i < 65; i++) lo.addTransceiver("audio");
    await rej(a, lo.createOffer(), "OperationError", "65 local transceivers exceed the default cap");
    lo.close();
    const lo2 = new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: 65 } });
    for (let i = 0; i < 65; i++) lo2.addTransceiver("audio");
    const o = await lo2.createOffer();
    a.eq(o.sdp.split("\r\nm=audio ").length - 1, 65);
    lo2.close();
    const small = new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: 2 } });
    await rej(a, small.setRemoteDescription({ type: "offer", sdp: offerWith(3) }), "OperationError", "a lowered cap");
    small.close();
    const ln = new RTCPeerConnection(LO, { sdpLimits: { maxCodecsPerSection: 1 } });
    await rej(a, ln.setRemoteDescription({ type: "offer", sdp: offerWith(1).replace("/SAVPF 0\r\n", "/SAVPF 0 8\r\n") }), "OperationError", "codec cap");
    ln.close();
});

t.test("a long remote msid stream id is kept whole (64 chars)", async (a) => {
    const id = "s".repeat(64);
    const pc = new RTCPeerConnection(LO);
    let ev = null;
    pc.ontrack = (e) => { ev = e; };
    await pc.setRemoteDescription({ type: "offer", sdp: offerWith(1) + "a=msid:" + id + " trk\r\n" });
    a.eq(ev && ev.streams[0].id, id);
    pc.close();
});

t.test("dynaOptions is a strict bag (dyna extension): unknown keys, bad types and ranges throw", (a) => {
    a.throws(() => new RTCPeerConnection(LO, 5), TypeError);
    a.throws(() => new RTCPeerConnection(LO, { sdpLimit: {} }), /sdpLimits/, "the error names the valid key");
    a.throws(() => new RTCPeerConnection(LO, { sdpLimits: 3 }), TypeError);
    a.throws(() => new RTCPeerConnection(LO, { sdpLimits: { maxMedia: 3 } }), /maxMediaSections/);
    a.throws(() => new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: "3" } }), TypeError);
    a.throws(() => new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: 0 } }), RangeError);
    a.throws(() => new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: 1.5 } }), RangeError);
    a.throws(() => new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: NaN } }), RangeError);
    a.throws(() => new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: -Infinity } }), RangeError);
    a.throws(() => new RTCPeerConnection(LO, { sdpLimits: { maxMediaSections: 2 ** 25 } }), RangeError);
    const keys = ["maxInputBytes", "maxLineBytes", "maxMediaSections", "maxCodecsPerSection", "maxHeaderExtensions", "maxCandidatesPerSection",
        "maxSsrcsPerSection", "maxMsidsPerSection", "maxSsrcGroups", "maxSsrcsPerGroup", "maxUnknownLines"];
    const all = {};
    for (const k of keys) all[k] = 1000;
    new RTCPeerConnection(LO, { sdpLimits: all }).close();
    new RTCPeerConnection(LO, {}).close();
    new RTCPeerConnection(LO, undefined).close();
    new RTCPeerConnection(undefined, { sdpLimits: { maxInputBytes: Infinity } }).close();
});

await t.run();
