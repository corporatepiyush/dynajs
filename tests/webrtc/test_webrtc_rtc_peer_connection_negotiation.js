// covers: dyna:webrtc.RTCPeerConnection -- re-offer payload types / extmap ids, simulcast answers, rollback of a remote re-offer, encoding codecs
// dyna:webrtc media renegotiation rules between two in-process RTCPeerConnections (no media flow), written by hand from
// the intent of the WPT webrtc tests (RTCPeerConnection-setRemoteDescription-rollback, RTCRtpParameters-codec,
// simulcast/negotiation, protocol/rtp-payloadtypes and rtp-headerextensions), no download:
//   - a subsequent offer keeps every negotiated payload type and extmap id (RFC 8829 5.2.2): a remote offerer that used
//     other numbers than our defaults (opus 109, VP8 120/rtx 121, mid id 9) gets them back in our re-offer; when the
//     remote gave one of our default numbers to another codec, ours moves to a free dynamic number, never a duplicate,
//     and each rtx apt follows its codec;
//   - the same holds across m= sections (BUNDLE shares one payload type space): a number negotiated for opus is never
//     reused by a video codec added later, and an extmap id negotiated for one URI moves every other URI off it;
//   - a simulcast offer (a=rid send + a=simulcast:send, RFC 8853 / RFC 8851) is answered with a=rid recv lines and
//     a=simulcast:recv mirroring the list; an answer that does not receive has none;
//   - the offerer applies the answer (webrtc-pc 4.4.1.6): encodings whose rid the answer's a=simulcast:recv omits are
//     removed, and an answer without simulcast leaves only the first encoding;
//   - rollback of a remote re-offer restores what the offer changed: direction and currentDirection of a transceiver the
//     offer stopped, and the remote stream membership of the receiver track (removetrack/addtrack again). What the
//     application changed meanwhile (direction, stop()) is kept. Deviation kept:
//     a receiver track the offer ended stays ended (a MediaStreamTrack never comes back to "live");
//   - RTCRtpEncodingParameters.codec: addTransceiver rejects a codec outside getCapabilities with OperationError;
//     setParameters checks against the negotiated codecs, else the codec preferences, else the capabilities
//     (InvalidModificationError); a codec the answer did not keep is removed from the encoding.
import { RTCPeerConnection, RTCRtpSender, RTCRtpReceiver, MediaStream } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { LO, negotiate, closeAll } from "./rtc_util.js";

const t = suite("webrtc_rtp_negotiation");

function track(kind) {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver(kind).receiver.track;
    pc.close();
    return tr;
}
async function rej(a, p, name, msg) {
    try { await p; a.ok(false, (msg || name) + ": did not reject"); }
    catch (e) { a.eq(e && e.name, name, (msg || "") + " " + (e && e.message)); }
}
function thr(a, fn, name, msg) {
    try { fn(); a.ok(false, (msg || name) + ": did not throw"); }
    catch (e) { a.eq(e && e.name, name, msg); }
}
function sections(sdp) { return sdp.split(/\r\n(?=m=)/).slice(1); }
function rtpmap(sec) {
    const out = {};
    for (const m of sec.matchAll(/\r\na=rtpmap:(\d+) ([^\r]+)/g)) out[m[1]] = m[2];
    return out;
}
function fmtp(sec, pt) { const m = new RegExp("\\r\\na=fmtp:" + pt + " ([^\\r]*)").exec(sec); return m ? m[1] : ""; }
function extmap(sec) {
    const out = {};
    for (const m of sec.matchAll(/\r\na=extmap:(\d+) (\S+)/g)) out[m[2]] = +m[1];
    return out;
}
function ptOf(sec, name, fm) {
    const map = rtpmap(sec);
    return +Object.keys(map).find((pt) => map[pt].startsWith(name + "/") && (!fm || fmtp(sec, pt).includes(fm)));
}
function remap(sdp, pts, ids) {
    const p = (n) => (n in pts ? pts[n] : n);
    return sdp.split("\r\n").map((l) => {
        let m;
        if ((m = /^(m=\S+ \d+ \S+) (.*)$/.exec(l))) return m[1] + " " + m[2].split(" ").map(p).join(" ");
        if ((m = /^a=(rtpmap|fmtp|rtcp-fb):(\d+)( .*)$/.exec(l))) return "a=" + m[1] + ":" + p(m[2]) + m[3].replace(/apt=(\d+)/, (_, x) => "apt=" + p(x));
        if ((m = /^a=extmap:(\d+)( .*)$/.exec(l))) return "a=extmap:" + (m[1] in ids ? ids[m[1]] : m[1]) + m[2];
        return l;
    }).join("\r\n");
}
const MID = "urn:ietf:params:rtp-hdrext:sdes:mid";

t.test("re-offer keeps the payload types and extmap ids a remote offerer chose (RFC 8829 5.2.2)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.addTrack(track("audio"));
    x.addTrack(track("video"));
    const o = await x.createOffer();
    const ro = remap(o.sdp, { 111: 109, 96: 120, 97: 121 }, { 1: 9 });
    await y.setRemoteDescription({ type: "offer", sdp: ro });
    const ans = await y.createAnswer();
    await y.setLocalDescription(ans);
    const [aa, av] = sections(ans.sdp);
    a.eq(ptOf(aa, "opus"), 109, "the answer uses the offer's numbers");
    a.eq(extmap(av)[MID], 9);
    const re = await y.createOffer();
    const [ra, rv] = sections(re.sdp);
    a.eq(ptOf(ra, "opus"), 109, "opus keeps 109");
    a.eq(ptOf(rv, "VP8"), 120, "VP8 keeps 120");
    const rtx = Object.keys(rtpmap(rv)).filter((pt) => rtpmap(rv)[pt].startsWith("rtx/"));
    a.ok(rtx.some((pt) => +pt === 121 && fmtp(rv, pt) === "apt=120"), "VP8 rtx keeps 121 with apt=120");
    a.eq(extmap(ra)[MID], 9, "mid keeps id 9 in audio");
    a.eq(extmap(rv)[MID], 9, "and in video");
    for (const sec of [ra, rv]) {
        const ids = Object.values(extmap(sec));
        a.eq(new Set(ids).size, ids.length, "extmap ids are unique");
    }
    a.eq(ptOf(ra, "PCMU"), 0, "codecs the answer kept as defaults stay");
    a.eq(ptOf(rv, "H264", "42e01f"), 98, "codecs that do not collide keep their default numbers");
    await y.setLocalDescription(re);
    await x.setRemoteDescription({ type: "offer", sdp: remap(re.sdp, { 109: 111, 120: 96, 121: 97 }, { 9: 1 }) });
    await x.setLocalDescription(await x.createAnswer());
    closeAll(x, y);
});

t.test("re-offer moves a default number the remote gave to another codec, rtx apt follows (no duplicate payload type)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.addTrack(track("video"));
    const o = await x.createOffer();
    const ro = remap(o.sdp, { 96: 98, 97: 99, 98: 96, 99: 97 }, { 1: 4, 4: 1 });
    await y.setRemoteDescription({ type: "offer", sdp: ro });
    await y.setLocalDescription(await y.createAnswer());
    const [rv] = sections((await y.createOffer()).sdp);
    const map = rtpmap(rv), pts = Object.keys(map).map(Number);
    a.eq(new Set(pts).size, pts.length, "payload types are unique");
    a.eq(ptOf(rv, "VP8"), 98, "VP8 keeps the negotiated 98");
    a.eq(ptOf(rv, "H264", "42e01f"), 96, "H264 42e01f keeps the negotiated 96");
    for (const pt of pts.filter((p) => map[p].startsWith("rtx/"))) {
        const apt = +/apt=(\d+)/.exec(fmtp(rv, pt))[1];
        a.ok(pts.includes(apt) && !map[apt].startsWith("rtx/"), "rtx " + pt + " points at a codec of this section");
    }
    const vp8rtx = pts.find((p) => map[p].startsWith("rtx/") && fmtp(rv, p) === "apt=98");
    a.eq(vp8rtx, 99, "VP8 rtx keeps the negotiated 99");
    const h264b = ptOf(rv, "H264", "42001f");
    a.ok(h264b !== 96 && h264b !== 98, "H264 42001f keeps a number of its own, not 96 or 98 (" + h264b + ")");
    const ids = extmap(rv);
    a.eq(ids[MID], 4, "mid keeps id 4");
    a.eq(ids["urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"], 1, "rid keeps id 1");
    a.eq(new Set(Object.values(ids)).size, Object.values(ids).length, "extmap ids are unique");
    closeAll(x, y);
});

t.test("re-offer: a negotiated number in another section moves our default (cross-section PT and extmap collision)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.addTrack(track("audio"));
    const o = await x.createOffer();
    await y.setRemoteDescription({ type: "offer", sdp: remap(o.sdp, { 111: 96 }, { 1: 4, 3: 5 }) });
    await y.setLocalDescription(await y.createAnswer());
    y.addTransceiver("video");
    const [ra, rv] = sections((await y.createOffer()).sdp);
    a.eq(ptOf(ra, "opus"), 96, "opus keeps the negotiated 96");
    const vp8 = ptOf(rv, "VP8");
    a.ne(vp8, 96, "VP8 leaves 96 to opus (BUNDLE: one payload type, one codec)");
    a.ok(vp8 >= 96 && vp8 <= 127, "a dynamic number (" + vp8 + ")");
    const map = rtpmap(rv), pts = Object.keys(map).map(Number);
    a.ok(!pts.includes(96), "96 is not reused in video");
    a.eq(new Set(pts).size, pts.length);
    const rtx = pts.find((p) => map[p].startsWith("rtx/") && fmtp(rv, p) === "apt=" + vp8);
    a.ok(rtx !== undefined && rtx !== 96, "VP8 rtx follows its codec (apt=" + vp8 + ")");
    a.eq(pts.filter((p) => map[p].startsWith("rtx/")).length, 3, "every rtx still has its codec");
    const ids = extmap(rv);
    a.eq(ids[MID], 4, "mid uses the negotiated id 4 in the new section too");
    a.ne(ids["urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"], 4, "rid moves off id 4");
    a.ne(ids["urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id"], 5, "repaired-rid moves off 5, which audio-level holds in the audio section");
    a.eq(new Set(Object.values(ids)).size, Object.values(ids).length, "extmap ids are unique");
    a.ok(Object.values(ids).every((i) => i >= 1 && i <= 14), "one-byte header ids");
    closeAll(x, y);
});

t.test("re-offer: an extension we add later never takes an id the remote gave to another URI", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.addTrack(track("video"));
    const o = await x.createOffer();
    const sdp = remap(o.sdp, {}, { 5: 4 }).split("\r\n").filter((l) => !/sdes:rtp-stream-id$/.test(l)).join("\r\n");
    await y.setRemoteDescription({ type: "offer", sdp });
    await y.setLocalDescription(await y.createAnswer());
    const ids = extmap(sections((await y.createOffer()).sdp)[0]);
    a.eq(ids["urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id"], 4, "repaired-rid keeps the negotiated 4");
    a.ne(ids["urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"], 4, "rid, not negotiated, does not take its default 4");
    a.eq(new Set(Object.values(ids)).size, Object.values(ids).length, "extmap ids are unique");
    closeAll(x, y);
});

t.test("simulcast answer prunes the offerer's encodings: missing rids removed, no simulcast leaves the first", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const tr = x.addTransceiver(track("video"), { sendEncodings: [{ rid: "h" }, { rid: "m" }, { rid: "l" }] });
    await x.setLocalDescription(await x.createOffer());
    await y.setRemoteDescription(x.localDescription);
    const ans = await y.createAnswer();
    const cut = ans.sdp.replace("a=rid:m recv\r\n", "").replace("a=simulcast:recv h;m;l", "a=simulcast:recv h;~l");
    await x.setRemoteDescription({ type: "answer", sdp: cut });
    a.deep(tr.sender.getParameters().encodings.map((e) => e.rid), ["h", "l"], "m is gone, a paused l stays");
    const re = await x.createOffer();
    a.match(re.sdp, /\r\na=simulcast:send h;l\r\n/, "the next offer lists the two left");
    const z = new RTCPeerConnection(LO), w = new RTCPeerConnection(LO);
    const t2 = z.addTransceiver(track("video"), { sendEncodings: [{ rid: "h", maxBitrate: 9 }, { rid: "l" }] });
    await z.setLocalDescription(await z.createOffer());
    await w.setRemoteDescription(z.localDescription);
    const plain = (await w.createAnswer()).sdp.split("\r\n").filter((l) => !/^a=(rid|simulcast):/.test(l)).join("\r\n");
    await z.setRemoteDescription({ type: "answer", sdp: plain });
    const e = t2.sender.getParameters().encodings;
    a.eq(e.length, 1, "an answer without simulcast keeps only the first encoding");
    a.eq(e[0].maxBitrate, 9, "the first encoding's values stay");
    a.ok(!/a=simulcast/.test((await z.createOffer()).sdp));
    closeAll(x, y, z, w);
});

t.test("rollback keeps what the application changed after the remote offer (direction, stop)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.addTrack(track("audio"));
    x.addTrack(track("video"));
    await negotiate(x, y);
    const [xa, xv] = x.getTransceivers();
    const cur = xa.currentDirection;
    y.getTransceivers()[0].direction = "sendonly";
    await x.setRemoteDescription(await y.createOffer());
    xa.direction = "inactive";
    xv.stop();
    await x.setRemoteDescription({ type: "rollback" });
    a.eq(xa.direction, "inactive", "a direction set during have-remote-offer survives rollback");
    a.eq(xv.direction, "stopped", "a stop() during have-remote-offer survives rollback");
    a.eq(xv.receiver.track.readyState, "ended");
    a.eq(xa.currentDirection, cur, "currentDirection is what was negotiated before");
    closeAll(x, y);
});

t.test("first offers use the default numbers; our own re-offer is unchanged after a round trip", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.addTrack(track("audio"));
    x.addTrack(track("video"));
    const o1 = await x.createOffer();
    await negotiate(x, y);
    const o2 = await x.createOffer();
    const strip = (s) => s.split("\r\n").filter((l) => /^(m=|a=rtpmap|a=fmtp|a=extmap)/.test(l)).join("\n");
    a.eq(strip(o2.sdp), strip(o1.sdp));
    closeAll(x, y);
});

t.test("simulcast offer is answered with a=rid recv and a=simulcast:recv (RFC 8853, webrtc-pc 5.4.1)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const tr = x.addTransceiver(track("video"), { sendEncodings: [{ rid: "h" }, { rid: "m" }, { rid: "l" }] });
    const o = await x.createOffer();
    a.match(o.sdp, /\r\na=simulcast:send h;m;l\r\n/);
    await x.setLocalDescription(o);
    await y.setRemoteDescription(o);
    const ans = await y.createAnswer();
    for (const r of ["h", "m", "l"]) a.match(ans.sdp, new RegExp("\\r\\na=rid:" + r + " recv\\r\\n"), "rid " + r);
    a.match(ans.sdp, /\r\na=simulcast:recv h;m;l\r\n/);
    a.ok(/urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id/.test(ans.sdp), "the rid header extension is accepted");
    await y.setLocalDescription(ans);
    await x.setRemoteDescription(ans);
    a.deep(tr.sender.getParameters().encodings.map((e) => e.rid), ["h", "m", "l"], "all three encodings stay");
    const z = new RTCPeerConnection(LO);
    await z.setRemoteDescription(o);
    z.getTransceivers()[0].direction = "inactive";
    const ans2 = await z.createAnswer();
    a.ok(!/a=simulcast|a=rid/.test(ans2.sdp), "an answer that does not receive has no simulcast");
    const p = remap(o.sdp, {}, {}).replace("a=simulcast:send h;m;l", "a=simulcast:send h;~m,q;l").replace("a=rid:l send", "a=rid:l recv");
    const w = new RTCPeerConnection(LO);
    await w.setRemoteDescription({ type: "offer", sdp: p });
    const ans3 = await w.createAnswer();
    a.match(ans3.sdp, /\r\na=simulcast:recv h;~m\r\n/, "a paused rid keeps ~; a rid without a send line is dropped");
    a.ok(!/a=rid:l|a=rid:q/.test(ans3.sdp));
    closeAll(x, y, z, w);
});

t.test("rollback of a remote re-offer restores direction, currentDirection and stopped state", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    x.addTrack(track("audio"));
    x.addTrack(track("video"));
    await negotiate(x, y);
    y.getTransceivers()[1].direction = "sendrecv";
    await negotiate(y, x);
    const xv = x.getTransceivers()[1];
    a.eq(xv.currentDirection, "sendrecv");
    y.getTransceivers()[1].stop();
    const o = await y.createOffer();
    a.match(o.sdp, /\r\nm=video 0 /, "the stopped transceiver's m= section is rejected");
    await x.setRemoteDescription(o);
    a.eq(xv.direction, "stopped", "the remote rejection stops the transceiver");
    a.eq(xv.currentDirection, "stopped");
    await x.setRemoteDescription({ type: "rollback" });
    a.eq(x.signalingState, "stable");
    a.eq(xv.direction, "sendrecv", "rollback restores direction");
    a.eq(xv.currentDirection, "sendrecv", "and currentDirection");
    a.eq(x.getTransceivers().length, 2, "the transceiver is still listed");
    const ao = await y.createOffer();
    await x.setRemoteDescription(ao);
    a.eq(xv.currentDirection, "stopped", "applying the offer again stops it again");
    await x.setLocalDescription(await x.createAnswer());
    closeAll(x, y);
});

t.test("rollback of a remote re-offer restores remote stream membership (removetrack/addtrack)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const s1 = new MediaStream();
    const sender = x.addTrack(track("audio"), s1);
    let rs = null;
    y.ontrack = (e) => { rs = e.streams[0]; };
    await negotiate(x, y);
    const rt = y.getTransceivers()[0].receiver.track;
    a.eq(rs.getTracks()[0], rt);
    const s2 = new MediaStream();
    sender.setStreams(s2);
    const o = await x.createOffer();
    const evs = [];
    rs.onremovetrack = () => evs.push("remove");
    rs.onaddtrack = () => evs.push("add");
    await y.setRemoteDescription(o);
    a.eq(rs.getTracks().length, 0, "the re-offer moved the track to another stream");
    await y.setRemoteDescription({ type: "rollback" });
    a.eq(rs.getTracks()[0], rt, "rollback puts it back");
    a.deep(evs, ["remove", "add"]);
    a.eq(rt.readyState, "live");
    await y.setRemoteDescription(o);
    a.eq(rs.getTracks().length, 0, "applying the same offer again moves it out again (the receiver's stream list was restored)");
    a.deep(evs, ["remove", "add", "remove"]);
    await y.setLocalDescription(await y.createAnswer());
    a.eq(rs.getTracks().length, 0);
    closeAll(x, y);
});

t.test("RTCRtpEncodingParameters.codec: addTransceiver OperationError, getParameters round trip", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const caps = RTCRtpSender.getCapabilities("video").codecs;
    const h264 = caps.find((c) => c.mimeType === "video/H264");
    const tr = pc.addTransceiver("video", { sendEncodings: [{ codec: h264 }] });
    a.deep(tr.sender.getParameters().encodings[0].codec, h264);
    thr(a, () => pc.addTransceiver("video", { sendEncodings: [{ codec: { mimeType: "video/AV1", clockRate: 90000 } }] }), "OperationError");
    thr(a, () => pc.addTransceiver("audio", { sendEncodings: [{ codec: h264 }] }), "OperationError", "a video codec on audio");
    thr(a, () => pc.addTransceiver("audio", { sendEncodings: [{ codec: { mimeType: "audio/opus", clockRate: 48000 } }] }), "OperationError", "channels must match");
    a.throws(() => pc.addTransceiver("video", { sendEncodings: [{ codec: { clockRate: 90000 } }] }), TypeError, "mimeType is required");
    const au = pc.addTransceiver("audio", { sendEncodings: [{ codec: RTCRtpReceiver.getCapabilities("audio").codecs[1] }] });
    a.eq(au.sender.getParameters().encodings[0].codec.mimeType, "audio/PCMU");
    pc.close();
});

t.test("setParameters codec: capabilities before negotiation, preferences, then negotiated codecs; answer drops it", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const caps = RTCRtpSender.getCapabilities("video").codecs;
    const vp8 = caps.find((c) => c.mimeType === "video/VP8"), h264 = caps.filter((c) => c.mimeType === "video/H264");
    const tr = x.addTransceiver(track("video"));
    let p = tr.sender.getParameters();
    p.encodings[0].codec = h264[1];
    await tr.sender.setParameters(p);
    a.deep(tr.sender.getParameters().encodings[0].codec, h264[1], "any capability before negotiation");
    tr.setCodecPreferences([vp8]);
    p = tr.sender.getParameters();
    p.encodings[0].codec = h264[0];
    await rej(a, tr.sender.setParameters(p), "InvalidModificationError", "outside the preferences");
    p = tr.sender.getParameters();
    p.encodings[0].codec = vp8;
    await tr.sender.setParameters(p);
    tr.setCodecPreferences([]);
    p = tr.sender.getParameters();
    p.encodings[0].codec = h264[0];
    await tr.sender.setParameters(p);
    await x.setLocalDescription(await x.createOffer());
    await y.setRemoteDescription(x.localDescription);
    y.getTransceivers()[0].setCodecPreferences([vp8]);
    await y.setLocalDescription(await y.createAnswer());
    await x.setRemoteDescription(y.localDescription);
    p = tr.sender.getParameters();
    a.eq(p.codecs.filter((c) => c.mimeType !== "video/rtx").map((c) => c.mimeType).join(), "video/VP8", "only VP8 negotiated");
    a.eq(p.encodings[0].codec, undefined, "the answer did not keep H264, so the encoding codec is removed");
    p.encodings[0].codec = h264[0];
    await rej(a, tr.sender.setParameters(p), "InvalidModificationError", "outside the negotiated codecs");
    p = tr.sender.getParameters();
    const neg = p.codecs.find((c) => c.mimeType === "video/VP8");
    p.encodings[0].codec = { mimeType: neg.mimeType, clockRate: neg.clockRate };
    await tr.sender.setParameters(p);
    a.eq(tr.sender.getParameters().encodings[0].codec.mimeType, "video/VP8");
    closeAll(x, y);
});

await t.run();
