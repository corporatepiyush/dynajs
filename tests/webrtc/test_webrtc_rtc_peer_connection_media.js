// flags: --std
// covers: dyna:webrtc.RTCPeerConnection -- media negotiation between two in-process peers (audio + video + data in one BUNDLE)
// dyna:webrtc media negotiation between two in-process RTCPeerConnections over loopback: audio + video + data in one
// BUNDLE group (RFC 9143) on one transport, media-only connections, a data channel added to a media-only connection by
// renegotiation, remote track processing (webrtc-pc 4.4.1.6: mute -> removetrack -> addtrack -> track, all before
// setRemoteDescription resolves), handlers that close the connection or add transceivers from inside those events, and
// a stop/renegotiate loop whose transceiver list and live object count must plateau (peak RSS is printed) (removed transceivers are released).
// Media flow (RTP/SRTP) is not part of this suite: negotiation and objects only.
import * as sys from "dyna:sys";
import { RTCPeerConnection, MediaStream } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { LO, trickle, negotiate, waitEvent, waitState, closeAll, delay } from "./rtc_util.js";

const t = suite("webrtc_media");

function track(kind) {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver(kind).receiver.track;
    pc.close();
    return tr;
}
function mids(sdp) { return [...sdp.matchAll(/\r\na=mid:(\S+)/g)].map((m) => m[1]); }
function bundle(sdp) { const m = /\r\na=group:BUNDLE ([^\r]*)/.exec(sdp); return m ? m[1].split(" ") : []; }

t.test("audio + video + data in one BUNDLE: connected, data round trip, sendrecv both ways", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    const sx = new MediaStream();
    x.addTrack(track("audio"), sx);
    x.addTrack(track("video"), sx);
    const dc = x.createDataChannel("chat");
    const got = [];
    y.ontrack = (e) => got.push(e.track.kind + ":" + e.streams.map((s) => s.id).join(","));
    y.ondatachannel = (e) => { e.channel.onmessage = (m) => e.channel.send("echo:" + m.data); };
    const o = await x.createOffer();
    a.deep(mids(o.sdp), ["0", "1", "2"]);
    a.deep(bundle(o.sdp), ["0", "1", "2"], "one BUNDLE group over every m= section");
    await x.setLocalDescription(o);
    await y.setRemoteDescription(o);
    a.deep(got, ["audio:" + sx.id, "video:" + sx.id], "track events with the remote stream");
    y.addTrack(track("audio"));
    y.addTrack(track("video"));
    const ys = y.getTransceivers();
    a.eq(ys.length, 2, "the addTrack calls reuse the offer's transceivers");
    a.eq(ys[0].direction, "sendrecv");
    const ans = await y.createAnswer();
    a.deep(bundle(ans.sdp), ["0", "1", "2"]);
    a.eq(new Set([...ans.sdp.matchAll(/a=ice-ufrag:(\S+)/g)].map((m) => m[1])).size, 1, "one set of ICE credentials");
    await y.setLocalDescription(ans);
    await x.setRemoteDescription(ans);
    a.deep(x.getTransceivers().map((tr) => tr.currentDirection), ["sendrecv", "sendrecv"]);
    a.deep(y.getTransceivers().map((tr) => tr.currentDirection), ["sendrecv", "sendrecv"]);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    if (dc.readyState !== "open") await waitEvent(dc, "open");
    const reply = waitEvent(dc, "message");
    dc.send("hi");
    a.eq((await reply).data, "echo:hi", "data channel works with an audio-tagged bundle");
    closeAll(x, y);
}, { timeoutMs: 20000 });

t.test("a media-only connection reaches connected; a data channel added later opens after renegotiation", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    x.addTransceiver("audio");
    await negotiate(x, y);
    a.eq(x.sctp, null, "no SCTP without an application m= section");
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    a.eq(x.connectionState, "connected");
    const nn = waitEvent(x, "negotiationneeded", 3000);
    const dc = x.createDataChannel("late");
    await nn;
    const remote = waitEvent(y, "datachannel", 10000);
    await negotiate(x, y);
    a.deep(mids(x.localDescription.sdp), ["0", "1"]);
    const rc = (await remote).channel;
    if (dc.readyState !== "open") await waitEvent(dc, "open");
    const m = waitEvent(rc, "message");
    dc.send("late hello");
    a.eq((await m).data, "late hello");
    closeAll(x, y);
}, { timeoutMs: 25000 });

t.test("remote track processing order: removetrack, addtrack, then track; all before setRemoteDescription resolves", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const s1 = new MediaStream(), s2 = new MediaStream();
    const ta = x.addTransceiver(track("audio"), { streams: [s1] });
    x.addTransceiver(track("video"), { streams: [s2] });
    const log = [], hooked = new Set();
    const tag = (s) => (s.id === s1.id ? "s1" : s.id === s2.id ? "s2" : "s?");
    y.ontrack = (e) => {
        log.push("track:" + e.track.kind);
        for (const rs of e.streams) {
            if (hooked.has(rs)) continue;
            hooked.add(rs);
            rs.onaddtrack = (ev) => log.push("addtrack:" + tag(rs) + ":" + ev.track.kind);
            rs.onremovetrack = (ev) => log.push("removetrack:" + tag(rs) + ":" + ev.track.kind);
        }
    };
    await negotiate(x, y);
    a.deep(log, ["track:audio", "track:video"]);
    a.eq(y.getReceivers()[0].track.muted, true, "remote tracks stay muted without media flow");
    log.length = 0;
    ta.sender.setStreams(s2);
    const tv = x.addTransceiver(track("video"), { streams: [s1] });
    void tv;
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    const states = [];
    y.addEventListener("track", () => states.push(y.signalingState));
    await y.setRemoteDescription(o);
    a.deep(log, ["removetrack:s1:audio", "addtrack:s2:audio", "addtrack:s1:video", "track:video"], "removetrack, then addtrack (the new video joins the known s1), then track");
    a.deep(states, ["have-remote-offer"], "fired inside setRemoteDescription(offer)");
    await y.setLocalDescription(await y.createAnswer());
    await x.setRemoteDescription(y.localDescription);
    log.length = 0;
    ta.direction = "inactive";
    await negotiate(x, y);
    a.deep(log, ["removetrack:s2:audio"], "a remote that stops sending removes the track from its streams");
    a.eq(y.getTransceivers()[0].currentDirection, "inactive");
    log.length = 0;
    ta.direction = "sendrecv";
    await negotiate(x, y);
    a.deep(log, ["addtrack:s2:audio", "track:audio"], "sending again re-adds the track to its stream, then fires track");
    closeAll(x, y);
});

t.test("handlers that close the connection or add transceivers inside track/addtrack/mute are safe", async (a) => {
    for (const mode of ["close", "add", "both"]) {
        const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
        const s = new MediaStream();
        for (let i = 0; i < 4; i++) x.addTransceiver(i & 1 ? "video" : "audio", { streams: [s] });
        let n = 0;
        y.ontrack = (e) => {
            n++;
            if (mode !== "close") y.addTransceiver("video");
            if (mode !== "add") y.close();
            e.streams[0].onaddtrack = () => { y.addTransceiver("audio"); };
        };
        const o = await x.createOffer();
        await x.setLocalDescription(o);
        const srd = y.setRemoteDescription(o);
        const settled = await Promise.race([srd.then(() => "ok", (e) => "err:" + e.name), delay(200).then(() => "pending")]);
        a.eq(settled, mode === "add" ? "ok" : "pending", mode + ": an operation whose own events closed the connection never settles");
        a.ok(n >= 1, mode + ": at least one track event");
        if (mode !== "add") a.eq(n, 1, mode + ": no events after close()");
        if (mode === "add") {
            a.eq(n, 4);
            const ans = await y.createAnswer();
            a.eq(ans.sdp.split("\r\nm=").length - 1, 4, "transceivers added during SRD do not enter the answer");
        }
        closeAll(x, y);
    }
});

t.test("stop/renegotiate cycles release removed transceivers: list and live objects plateau", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const rss = [], objs = [];
    for (let i = 0; i < 300; i++) {
        const tr = x.addTransceiver(i & 1 ? "video" : "audio");
        await negotiate(x, y);
        tr.stop();
        await negotiate(x, y);
        if (i % 50 === 49) await delay(0);
        if (i % 50 === 49) { std.gc(); rss.push(sys.memoryUsage().peakRss); objs.push(sys.memoryUsage().objCount); }
    }
    a.eq(x.getTransceivers().length, 0, "every stopped transceiver left the offerer's list");
    a.eq(y.getTransceivers().length, 0, "and the answerer's");
    const sdpLines = x.localDescription.sdp.split("\r\nm=").length - 1;
    a.ok(sdpLines <= 300, "m= sections: " + sdpLines);
    console.log("  rss after 100..300 cycles (KiB): " + rss.map((v) => Math.round(v / 1024)).join(" ") + "; m= sections " + sdpLines);
    a.ok(objs[objs.length - 1] - objs[1] < 64, "live JS objects plateau after gc (no transceiver leak): " + objs.join(" "));
    closeAll(x, y);
}, { timeoutMs: 60000 });

await t.run();
