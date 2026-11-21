// flags: --std (run by tests/ctest/run.sh test-webrtc-media-e2e under tests/webrtc/webrtc_media_harness, not ./dynajs)
// covers: integration dyna:webrtc.RTCRtpReceiver dyna:webrtc.MediaStreamTrack -- W6 media path: harness origin -> RTP -> SRTP -> UDP -> demux -> SRTP unprotect -> BUNDLE routing -> receiver
// Two in-process RTCPeerConnections over loopback; the only media origin is the C harness global rtpHarness (see
// webrtc_media_harness.c), which drives the product send path exactly as a P3 encoder will. Spec points pinned:
//   - webrtc-pc 5.4 / 4.4.1.6: a muted, not ended remote track is unmuted (a task firing "unmute") on the first packet
//     only when the transceiver's [[Receptive]] slot is true: set by sLD from the local m= direction (recv in it),
//     cleared by sRD when the reversed remote direction is sendonly/inactive and by stop();
//   - webrtc-pc 5.4: a BYE for one of the receiver's SSRCs sets muted (a task firing "mute");
//   - webrtc-pc 5.3 getSynchronizationSources/getContributingSources: one entry per SSRC / CSRC seen in the last 10 s,
//     descending timestamp; source, rtpTimestamp, timestamp (wall-clock ms: performance.timeOrigin is not defined in
//     dynajs, so timestamp is comparable to Date.now()); audioLevel only for audio, from RFC 6464 (SSRC) and RFC 6465
//     (CSRC) with 127 -> 0 and 10^(-level/20) otherwise, absent without the extension value; packets are applied in
//     ascending RTP timestamp order (an older timestamp never overwrites a newer one);
//   - RFC 8843 9.2 routing: the MID header extension selects the receiver and latches the SSRC;
//   - RFC 3711: forged (unauthenticated) and replayed packets are dropped before routing, never unmute or add sources;
//   - RFC 3550 A.3 loss as seen by the receiver equals what the harness impairment dropped on the wire;
//   - both SRTP directions carry media; a keying oracle built in the harness from RFC 5764 4.2 (the exported block
//     split by DTLS role) must be accepted, so a symmetric key-direction swap cannot pass self-interop unnoticed;
//   - a sender sends only while currentDirection sends and it has a track; payload types outside the m= section are
//     dropped (counted); sRD of a recvonly re-offer mutes the track and clears [[Receptive]];
//   - sLD(offer) takes [[Receptive]] from the offer's m= direction (set at createOffer), not from the transceiver
//     direction at sLD time;
//   - RFC 3550 6.3.5: a receiver with no RTP (or RTCP SR) from its source for M*Td = 5 * 5 s = 25 s is muted (a task
//     firing "mute"; harness age() moves the last-activity time back instead of waiting); the next packet unmutes;
//   - RFC 4733 2.3/2.5: insertDTMF sends telephone-event packets on the negotiated telephone-event payload type whose
//     clock matches the audio codec: event code 0-9 * # A-D = 0-15, marker on the first packet, one RTP timestamp per
//     tone, volume 10, duration in clock units = elapsed + 50 ms (capped at the tone), three end packets (E bit)
//     carrying the full duration; a tone longer than 0xFFFF units is split into segments (new timestamp, no marker);
//     ',' sends nothing;
//   - relay of a received track (webrtc-pc 5.2 addTrack/replaceTrack with a remote track): each packet the receiver
//     accepts is forwarded on the sender with the sender's SSRC, sequence and timestamp base, payload type mapped by
//     codec name + clock + channels (unmapped payload types are skipped, counted), CSRCs and RFC 6464/6465 bytes kept;
//     forwarding stops on replaceTrack(null) and when the relayed track ends (stop()); the map is rebuilt when either
//     side renegotiates; a tone on the relaying sender takes the next sequence numbers (no replay at the far end);
//     closing the destination pc first is safe. The harness, like an encoder, produces nothing from an ended track.
// No decoder exists (P3), so "delivered to the track" means accepted after SRTP unprotect and routing.
import { RTCPeerConnection, RTCRtpSender } from "dyna:webrtc";
import { UDPSocket } from "dyna:net";
import { suite } from "../lib/kit.js";
import { LO, trickle, negotiate, waitState, waitEvent, closeAll, delay } from "./rtc_util.js";

const t = suite("webrtc_media_e2e");
const H = globalThis.rtpHarness;

const held = [];
t.after(() => { for (const p of held) p.close(); });
function srcTrack(kind) {
    const pc = new RTCPeerConnection(LO);
    held.push(pc);
    return pc.addTransceiver(kind).receiver.track;
}
async function until(fn, ms = 5000) {
    const end = Date.now() + ms;
    while (Date.now() < end) { if (fn()) return true; await delay(5); }
    return !!fn();
}
async function pair(kinds = ["audio"], opts = {}) {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    const xs = kinds.map((k) => x.addTransceiver(srcTrack(k), { direction: "sendrecv" }));
    const ys = [];
    y.ontrack = (e) => { ys.push(e.transceiver); if (opts.ontrack) opts.ontrack(e); };
    await negotiate(x, y);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    const ry = xs.map((tr) => y.getTransceivers().find((u) => u.mid === tr.mid));
    return { x, y, xs, ry };
}
function ssrcOf(pc, mid) {
    const secs = pc.localDescription.sdp.split(/\r\nm=/);
    const s = secs.find((b) => b.includes("a=mid:" + mid + "\r\n"));
    const m = s && /a=ssrc:(\d+) /.exec(s);
    return m ? Number(m[1]) : -1;
}

t.test("origin -> SRTP -> receive: first packet unmutes the remote track (a task), stats count every packet", async (a) => {
    const { x, y, xs, ry } = await pair();
    const track = ry[0].receiver.track;
    a.eq(track.muted, true, "remote track starts muted");
    const evs = [];
    track.onmute = () => evs.push("mute");
    track.onunmute = () => evs.push("unmute:" + track.muted);
    a.eq(H.send(xs[0].sender, { count: 5, ts: 1000, tsStep: 960 }), 5);
    a.eq(track.muted, true, "not unmuted synchronously inside the sending task");
    a.ok(await until(() => evs.length > 0), "unmute fired");
    a.eq(evs.join(","), "unmute:false");
    a.ok(await until(() => H.stats(ry[0].receiver).pkts === 5), "5 packets received");
    const st = H.stats(ry[0].receiver);
    a.eq(st.ssrc, ssrcOf(x, xs[0].mid), "receiver latched the sender's SDP SSRC");
    a.eq(st.lost, 0);
    a.eq(H.counts(ry[0].receiver).srtpAuth, 0);
    H.send(xs[0].sender, { count: 3 });
    await until(() => H.stats(ry[0].receiver).pkts === 8);
    await delay(50);
    a.eq(evs.length, 1, "one unmute, not one per packet");
    closeAll(x, y);
});

t.test("getSynchronizationSources: source, rtpTimestamp, wall-clock timestamp; audioLevel per RFC 6464", async (a) => {
    const { x, y, xs, ry } = await pair();
    const r = ry[0].receiver;
    a.eq(r.getSynchronizationSources().length, 0, "nothing before media");
    const t0 = Date.now();
    H.send(xs[0].sender, { count: 1, ts: 5000 });
    await until(() => r.getSynchronizationSources().length === 1);
    let s = r.getSynchronizationSources()[0];
    a.eq(s.source, ssrcOf(x, xs[0].mid));
    a.ok(s.timestamp >= t0 - 50 && s.timestamp <= Date.now() + 50, "timestamp is wall-clock ms: " + (s.timestamp - t0));
    a.eq(Object.prototype.hasOwnProperty.call(s, "audioLevel"), false, "no RFC 6464 value -> audioLevel absent");
    const rtp0 = s.rtpTimestamp;
    let step = 0;
    for (const [lv, want] of [[0, 1], [127, 0], [128 | 20, 0.1], [6, Math.pow(10, -6 / 20)]]) {
        const before = H.stats(r).pkts;
        step += 10;
        H.send(xs[0].sender, { count: 1, ts: 6000 + step, level: lv });
        await until(() => H.stats(r).pkts === before + 1);
        s = r.getSynchronizationSources()[0];
        a.eq(s.audioLevel === undefined ? "absent" : "present", "present", "level " + lv);
        if (want === 0) a.eq(s.audioLevel, 0, "level 127 is exactly 0");
        else a.near(s.audioLevel, want, 1e-9, "level " + lv + " (bit 7 is the RFC 6464 V flag, not level)");
        a.eq((s.rtpTimestamp - rtp0) >>> 0, 1000 + step, "rtpTimestamp follows the packet");
    }
    const keep = s.rtpTimestamp, before = H.stats(r).pkts;
    H.send(xs[0].sender, { count: 1, ts: 100, level: 50 });
    await until(() => H.stats(r).pkts === before + 1);
    s = r.getSynchronizationSources()[0];
    a.eq(s.rtpTimestamp, keep, "an older RTP timestamp does not overwrite a newer one");
    a.near(s.audioLevel, Math.pow(10, -6 / 20), 1e-9);
    const s1 = r.getSynchronizationSources(), s2 = r.getSynchronizationSources();
    a.eq(JSON.stringify(s1), JSON.stringify(s2), "same values within one task");
    closeAll(x, y);
});

t.test("getContributingSources: one entry per CSRC, descending timestamp, RFC 6465 levels; video has no audioLevel", async (a) => {
    const { x, y, xs, ry } = await pair(["audio", "video"]);
    const r = ry[0].receiver, v = ry[1].receiver;
    H.send(xs[0].sender, { count: 1, ts: 10, csrc: [11, 22], clevels: [0, 127] });
    await until(() => r.getContributingSources().length === 2);
    await delay(20);
    H.send(xs[0].sender, { count: 1, ts: 20, csrc: [33] });
    await until(() => r.getContributingSources().length === 3);
    const cs = r.getContributingSources();
    a.eq(cs.map((c) => c.source).join(","), cs[0].source === 33 ? cs.map((c) => c.source).join(",") : "33,?", "newest first");
    a.eq(cs[0].source, 33);
    a.ok(cs[0].timestamp >= cs[1].timestamp && cs[1].timestamp >= cs[2].timestamp, "descending timestamp");
    const by = Object.fromEntries(cs.map((c) => [c.source, c]));
    a.near(by[11].audioLevel, 1, 1e-9, "CSRC level 0 -> 1");
    a.eq(by[22].audioLevel, 0, "CSRC level 127 -> 0");
    a.eq("audioLevel" in by[33], false, "no RFC 6465 value -> absent");
    a.eq(by[11].rtpTimestamp, (r.getSynchronizationSources()[0].rtpTimestamp - 10) >>> 0, "CSRC rtpTimestamp is the packet's");
    a.eq(r.getSynchronizationSources().length, 1, "CSRCs are not SSRCs");
    H.send(xs[1].sender, { count: 2, ts: 0, tsStep: 3000, level: 10, csrc: [44], clevels: [10] });
    await until(() => v.getSynchronizationSources().length === 1 && v.getContributingSources().length === 1);
    a.eq("audioLevel" in v.getSynchronizationSources()[0], false, "video SSRC: no audioLevel");
    a.eq("audioLevel" in v.getContributingSources()[0], false, "video CSRC: no audioLevel");
    closeAll(x, y);
});

t.test("sources older than 10 s are not returned", async (a) => {
    const { x, y, xs, ry } = await pair();
    const r = ry[0].receiver;
    H.send(xs[0].sender, { count: 1, ts: 1, csrc: [7] });
    await until(() => r.getSynchronizationSources().length === 1);
    await delay(9000);
    a.eq(r.getSynchronizationSources().length, 1, "still there at 9 s");
    await delay(1300);
    a.eq(r.getSynchronizationSources().length, 0, "gone after 10 s");
    a.eq(r.getContributingSources().length, 0);
    closeAll(x, y);
});

t.test("BUNDLE: MID routes each stream to its own receiver; no cross-talk", async (a) => {
    const { x, y, xs, ry } = await pair(["audio", "audio", "video"]);
    const want = [3, 5, 7];
    xs.forEach((tr, i) => H.send(tr.sender, { count: want[i], ts: 0, tsStep: 10 }));
    a.ok(await until(() => ry.every((u, i) => (H.stats(u.receiver) || {}).pkts === want[i])), "per-receiver counts");
    ry.forEach((u, i) => a.eq(H.stats(u.receiver).ssrc, ssrcOf(x, xs[i].mid), "ssrc of m=" + i));
    a.eq(H.counts(ry[0].receiver).unroutable, 0);
    a.ok(ry.every((u) => !u.receiver.track.muted), "all three unmuted");
    closeAll(x, y);
});

t.test("routing: the MID extension alone; signalled SSRCs alone (two m= share payload types); neither: the unique payload type", async (a) => {
    for (const [strip, n] of [[[/^a=ssrc/], 2], [[/sdes:mid/], 2], [[/^a=ssrc/, /sdes:mid/], 1]]) {
        const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
        trickle(x, y);
        const trs = [];
        for (let i = 0; i < n; i++) trs.push(x.addTransceiver(srcTrack("audio"), { direction: "sendrecv" }));
        await x.setLocalDescription(await x.createOffer());
        const sdp = x.localDescription.sdp.split("\r\n").filter((l) => !strip.some((re) => re.test(l))).join("\r\n");
        await y.setRemoteDescription({ type: "offer", sdp });
        await y.setLocalDescription(await y.createAnswer());
        await x.setRemoteDescription(y.localDescription);
        await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
        const rys = trs.map((tr) => y.getTransceivers().find((u) => u.mid === tr.mid));
        trs.forEach((tr, i) => a.eq(H.send(tr.sender, { count: 4 + i }), 4 + i));
        a.ok(await until(() => rys.every((u, i) => (H.stats(u.receiver) || {}).pkts === 4 + i)), "routed per m= with " + strip.length + " strip rule(s)");
        a.eq(H.counts(rys[0].receiver).unroutable, 0);
        a.ok(await until(() => rys.every((u) => !u.receiver.track.muted)), "unmuted");
        closeAll(x, y);
    }
});

t.test("[[Receptive]] false (local sendonly offer) keeps the track muted while packets still arrive", async (a) => {
    const { x, y, xs, ry } = await pair();
    const track = ry[0].receiver.track;
    let un = 0;
    track.onunmute = () => un++;
    ry[0].direction = "sendonly";
    await y.setLocalDescription(await y.createOffer());
    a.eq(H.send(xs[0].sender, { count: 3 }), 3, "x still sends (its currentDirection is sendrecv)");
    a.ok(await until(() => (H.stats(ry[0].receiver) || {}).pkts === 3), "packets reach the receiver");
    await delay(100);
    a.eq(un, 0, "no unmute");
    a.eq(track.muted, true);
    await y.setLocalDescription({ type: "rollback" });
    closeAll(x, y);
});

t.test("a remote track the application stopped (ended) is never unmuted by later packets", async (a) => {
    const { x, y, xs, ry } = await pair();
    const track = ry[0].receiver.track;
    let un = 0;
    track.onunmute = () => un++;
    track.stop();
    a.eq(track.readyState, "ended");
    H.send(xs[0].sender, { count: 3 });
    a.ok(await until(() => (H.stats(ry[0].receiver) || {}).pkts === 3), "packets still received");
    await delay(100);
    a.eq(un, 0);
    a.eq(track.muted, true);
    closeAll(x, y);
});

t.test("RTCP BYE mutes the track and drops the SSRC from the sources; the next packet unmutes again", async (a) => {
    const { x, y, xs, ry } = await pair();
    const track = ry[0].receiver.track, r = ry[0].receiver;
    H.send(xs[0].sender, { count: 2 });
    await waitEvent(track, "unmute", 5000);
    a.eq(H.bye(xs[0].sender), true);
    await waitEvent(track, "mute", 5000);
    a.eq(track.muted, true);
    a.eq(r.getSynchronizationSources().length, 0, "BYE removes the SSRC");
    a.eq(H.counts(r).byes, 1);
    H.send(xs[0].sender, { count: 1 });
    await waitEvent(track, "unmute", 5000);
    a.eq(r.getSynchronizationSources().length, 1);
    closeAll(x, y);
});

function ptOf(pc, name) {
    const m = new RegExp("a=rtpmap:(\\d+) " + name + "/", "i").exec(pc.localDescription.sdp);
    return m ? Number(m[1]) : -1;
}

t.test("answerer -> offerer: the other SRTP direction carries media, unmutes and fills sources on the offerer", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    const xt = x.addTransceiver(srcTrack("audio"), { direction: "sendrecv" });
    await x.setLocalDescription(await x.createOffer());
    await y.setRemoteDescription(x.localDescription);
    const yt = y.getTransceivers()[0];
    yt.direction = "sendrecv";
    await yt.sender.replaceTrack(srcTrack("audio"));
    await y.setLocalDescription(await y.createAnswer());
    await x.setRemoteDescription(y.localDescription);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    const track = xt.receiver.track;
    a.eq(track.muted, true);
    a.eq(H.send(yt.sender, { count: 6, ts: 70, level: 40, csrc: [5] }), 6);
    await waitEvent(track, "unmute", 5000);
    a.ok(await until(() => (H.stats(xt.receiver) || {}).pkts === 6), "offerer received 6");
    a.eq(xt.receiver.getSynchronizationSources()[0].source, ssrcOf(y, yt.mid));
    a.near(xt.receiver.getSynchronizationSources()[0].audioLevel, Math.pow(10, -2), 1e-9);
    a.eq(xt.receiver.getContributingSources()[0].source, 5);
    a.eq(H.send(xt.sender, { count: 2 }), 2, "and the offerer still sends");
    a.ok(await until(() => (H.stats(yt.receiver) || {}).pkts === 2));
    closeAll(x, y);
});

t.test("keying oracle: a packet protected with the RFC 5764 4.2 key half of the sender's DTLS role is accepted (both roles)", async (a) => {
    const { x, y, xs, ry } = await pair();
    const pt = ptOf(x, "opus");
    a.ok(pt > 0, "opus negotiated");
    H.oracle(xs[0].sender, pt, 0x1234567);
    a.ok(await until(() => H.counts(ry[0].receiver).rtpIn === 1), "answerer accepts the offerer-role packet");
    a.eq(H.counts(ry[0].receiver).srtpAuth, 0);
    H.oracle(ry[0].receiver, pt, 0x7654321);
    a.ok(await until(() => H.counts(xs[0].sender).rtpIn === 1), "offerer accepts the answerer-role packet");
    a.eq(H.counts(xs[0].sender).srtpAuth, 0);
    closeAll(x, y);
});

t.test("payload types: one outside the m= section is dropped and counted; it neither unmutes nor adds a source", async (a) => {
    const { x, y, xs, ry } = await pair();
    a.eq(H.send(xs[0].sender, { count: 2, pt: 77 }), 2);
    a.ok(await until(() => H.counts(ry[0].receiver).ptDrop === 2), "2 dropped by payload type");
    await delay(50);
    a.eq(ry[0].receiver.track.muted, true);
    a.eq(ry[0].receiver.getSynchronizationSources().length, 0);
    a.eq(H.stats(ry[0].receiver).pkts, 0);
    closeAll(x, y);
});

t.test("a sender sends only when currentDirection sends and it has a track (recvonly, replaceTrack(null))", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    const rec = x.addTransceiver(srcTrack("audio"), { direction: "recvonly" });
    const snd = x.addTransceiver(srcTrack("audio"), { direction: "sendrecv" });
    await negotiate(x, y);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    a.eq(H.send(rec.sender, { count: 1 }), 0, "recvonly with a track sends nothing");
    a.eq(H.send(snd.sender, { count: 1 }), 1);
    await snd.sender.replaceTrack(null);
    a.eq(H.send(snd.sender, { count: 1 }), 0, "no track, nothing sent");
    await snd.sender.replaceTrack(srcTrack("audio"));
    a.eq(H.send(snd.sender, { count: 1 }), 1, "a track again");
    closeAll(x, y);
});

t.test("sRD of a recvonly re-offer clears [[Receptive]]: the track is muted and later packets do not unmute it", async (a) => {
    const { x, y, xs, ry } = await pair();
    const track = ry[0].receiver.track;
    H.send(xs[0].sender, { count: 1 });
    await waitEvent(track, "unmute", 5000);
    xs[0].direction = "recvonly";
    await x.setLocalDescription(await x.createOffer());
    const muted = waitEvent(track, "mute", 5000);
    await y.setRemoteDescription(x.localDescription);
    await muted;
    let un = 0;
    track.onunmute = () => un++;
    const before = H.stats(ry[0].receiver).pkts;
    a.eq(H.send(xs[0].sender, { count: 3 }), 3, "x's currentDirection still sends until the answer");
    a.ok(await until(() => H.stats(ry[0].receiver).pkts === before + 3), "packets still routed");
    await delay(100);
    a.eq(un, 0);
    a.eq(track.muted, true);
    closeAll(x, y);
});

t.test("forged and replayed packets: dropped by SRTP, never unmute, never add a source", async (a) => {
    const { x, y, xs, ry } = await pair();
    const r = ry[0].receiver, ssrc = ssrcOf(x, xs[0].mid);
    const pkt = new Uint8Array(12 + 40 + 10);
    pkt.set([0x80, 111, 0, 1, 0, 0, 0, 9, ssrc >>> 24, (ssrc >>> 16) & 255, (ssrc >>> 8) & 255, ssrc & 255]);
    for (let i = 0; i < 5; i++) H.raw(xs[0].sender, pkt);
    a.ok(await until(() => H.counts(r).srtpAuth === 5), "5 authentication failures");
    await delay(50);
    a.eq(r.track.muted, true);
    a.eq(r.getSynchronizationSources().length, 0);
    H.impair(xs[0].sender, { dup: 1000000, seed: 3 });
    H.send(xs[0].sender, { count: 4 });
    a.ok(await until(() => H.counts(r).srtpReplay === 4), "every duplicate is a replay");
    a.eq(H.stats(r).pkts, 4, "duplicates are not counted as media");
    a.eq(H.counts(xs[0].sender).dup, 4);
    closeAll(x, y);
});

t.test("RTP from an address that is not the selected ICE peer is dropped before SRTP", async (a) => {
    const { x, y, xs, ry } = await pair();
    const r = ry[0].receiver, ssrc = ssrcOf(x, xs[0].mid);
    const ports = [...y.localDescription.sdp.matchAll(/a=candidate:\S+ 1 udp \d+ 127\.0\.0\.1 (\d+) typ host/g)].map((m) => Number(m[1]));
    a.ok(ports.length > 0, "y has a loopback host candidate");
    const sock = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const pkt = new Uint8Array(40);
    pkt.set([0x80, 111, 0, 1, 0, 0, 0, 9, ssrc >>> 24, (ssrc >>> 16) & 255, (ssrc >>> 8) & 255, ssrc & 255]);
    for (const p of ports) for (let i = 0; i < 3; i++) sock.send(pkt, "127.0.0.1", p);
    a.ok(await until(() => H.counts(r).rtpDropped >= 3), "off-path packets counted as dropped");
    a.eq(H.counts(r).srtpAuth, 0, "never reached SRTP");
    sock.close();
    closeAll(x, y);
});

t.test("loss and reordering injection: receiver loss equals the wire drops (RFC 3550 A.3)", async (a) => {
    const { x, y, xs, ry } = await pair();
    const r = ry[0].receiver;
    let sent = H.send(xs[0].sender, { count: 1, ts: 0 });
    await until(() => (H.stats(r) || {}).pkts === 1);
    H.impair(xs[0].sender, { loss: 200000, reorder: 100000, seed: 12345 });
    for (let i = 0; i < 20; i++) { sent += H.send(xs[0].sender, { count: 10, ts: i * 9600 }); await delay(2); }
    H.impair(xs[0].sender, {});
    sent += H.send(xs[0].sender, { count: 1, ts: 999999 });
    const c = H.counts(xs[0].sender);
    a.eq(sent, 202);
    a.ok(c.drop > 10 && c.drop < 80, "about 20% dropped: " + c.drop);
    a.ok(c.reorder > 3, "some reordered: " + c.reorder);
    a.ok(await until(() => H.stats(r).pkts === sent - c.drop), "received = sent - dropped");
    const st = H.stats(r);
    a.eq(st.lost, c.drop, "RFC 3550 cumulative loss equals the injected drops");
    a.eq(H.counts(r).srtpReplay + H.counts(r).srtpAuth, 0, "reordering inside the replay window is accepted");
    closeAll(x, y);
});

t.test("no media path for a stopped transceiver; a closed pc sends nothing", async (a) => {
    const { x, y, xs, ry } = await pair(["audio", "audio"]);
    ry[1].stop();
    H.send(xs[1].sender, { count: 3 });
    await delay(100);
    a.eq(H.stats(ry[1].receiver), null, "stopped receiver has no media state");
    a.eq(ry[1].receiver.getSynchronizationSources().length, 0);
    xs[0].stop();
    a.eq(H.send(xs[0].sender, { count: 1 }), 0, "stopped sender cannot send");
    x.close();
    a.eq(H.send(xs[1].sender, { count: 1 }), 0, "closed pc cannot send");
    closeAll(x, y);
});

t.test("[[Receptive]] comes from the offer's m= direction at sLD, not the transceiver direction (both ways)", async (a) => {
    for (const [atOffer, atSld, want] of [["sendrecv", "sendonly", true], ["sendonly", "sendrecv", false]]) {
        const { x, y, xs, ry } = await pair();
        const track = ry[0].receiver.track;
        let un = 0;
        track.onunmute = () => un++;
        ry[0].direction = atOffer;
        const offer = await y.createOffer();
        ry[0].direction = atSld;
        await y.setLocalDescription(offer);
        a.match(y.localDescription.sdp, new RegExp("a=" + atOffer + "\\r\\n"), "the m= line carries " + atOffer);
        a.eq(H.send(xs[0].sender, { count: 3 }), 3);
        a.ok(await until(() => (H.stats(ry[0].receiver) || {}).pkts === 3), "packets reach the receiver");
        if (want) a.ok(await until(() => un === 1), atOffer + " offer, " + atSld + " at sLD: unmuted");
        else { await delay(100); a.eq(un, 0, atOffer + " offer, " + atSld + " at sLD: stays muted"); }
        await y.setLocalDescription({ type: "rollback" });
        closeAll(x, y);
    }
});

t.test("RFC 3550 6.3.5 inactivity: 25 s without RTP mutes (a task); 24 s does not; the next packet unmutes", async (a) => {
    const { x, y, xs, ry } = await pair();
    const track = ry[0].receiver.track, r = ry[0].receiver;
    const evs = [];
    track.onmute = () => evs.push("mute:" + track.muted);
    track.onunmute = () => evs.push("unmute");
    H.send(xs[0].sender, { count: 2 });
    a.ok(await until(() => evs.length === 1), "unmuted");
    H.age(r, 24000);
    await delay(100);
    a.eq(evs.join(","), "unmute", "24 s idle: still unmuted");
    a.eq(H.counts(r).timeouts, 0);
    H.age(r, 1100);
    a.ok(await until(() => evs.length === 2), "25 s idle: mute fired");
    a.eq(evs.join(","), "unmute,mute:true");
    a.eq(H.counts(r).timeouts, 1);
    a.eq(track.readyState, "live", "a timeout mutes, it does not end");
    H.send(xs[0].sender, { count: 1, ts: 5000 });
    a.ok(await until(() => evs.length === 3), "next packet unmutes");
    a.eq(evs[2], "unmute");
    H.age(r, 30000);
    a.ok(await until(() => evs.length === 4), "times out again");
    a.eq(H.counts(r).timeouts, 2, "one timeout per idle period, not one per tick");
    await delay(100);
    a.eq(evs.length, 4);
    closeAll(x, y);
});

t.test("RFC 3550 6.3.5 inactivity: an RTCP sender report from the source counts as activity", async (a) => {
    const { x, y, xs, ry } = await pair();
    const track = ry[0].receiver.track, r = ry[0].receiver;
    const evs = [];
    track.onmute = () => evs.push("mute");
    track.onunmute = () => evs.push("unmute");
    H.send(xs[0].sender, { count: 1 });
    a.ok(await until(() => evs.length === 1), "unmuted");
    H.age(r, 24000);
    const in0 = H.counts(r).rtcpIn;
    a.eq(H.sr(xs[0].sender), true);
    a.ok(await until(() => H.counts(r).rtcpIn === in0 + 1), "SR received");
    H.age(r, 2000);
    await delay(100);
    a.eq(evs.join(","), "unmute", "24 s + 2 s idle, but an SR came in between: not muted");
    H.age(r, 23500);
    a.ok(await until(() => evs.length === 2), "25 s after the SR: muted");
    a.eq(evs[1], "mute");
    closeAll(x, y);
});
async function tones(a, xs, ry, buf, dur, settle) {
    const r = ry[0].receiver, d = xs[0].sender.dtmf;
    a.eq(d.canInsertDTMF, true);
    const before = H.stats(r) || { tePkts: 0, teEnds: 0, teMarkers: 0, teSegs: 0 };
    const out0 = H.counts(xs[0].sender).dtmfOut;
    const done = new Promise((res) => {
        const h = (e) => { if (e.tone === "") { d.removeEventListener("tonechange", h); res(); } };
        d.addEventListener("tonechange", h);
    });
    d.insertDTMF(buf, dur, 30);
    await done;
    await delay(settle);
    const s = H.stats(r);
    return { s, pkts: s.tePkts - before.tePkts, ends: s.teEnds - before.teEnds, markers: s.teMarkers - before.teMarkers,
        segs: s.teSegs - before.teSegs, out: H.counts(xs[0].sender).dtmfOut - out0 };
}

t.test("RFC 4733 on the wire: '1' 100 ms at 48 kHz -> event 1, marker once, duration 4800, three end packets", async (a) => {
    const { x, y, xs, ry } = await pair();
    const m = /a=rtpmap:(\d+) telephone-event\/48000/.exec(x.localDescription.sdp);
    a.ok(m !== null, "telephone-event/48000 offered next to opus/48000");
    const r = await tones(a, xs, ry, "1", 100, 150);
    a.eq(r.s.teEvent, 1);
    a.eq(r.s.teDur, 4800, "final duration = 100 ms * 48 units/ms");
    a.eq(r.s.teVol, 10);
    a.eq(r.s.teFirstDur, 2400, "the first update carries the duration through the next one (50 ms = 2400 units)");
    a.eq(r.markers, 1, "marker only on the first packet");
    a.eq(r.ends, 3, "end packet sent three times");
    a.eq(r.segs, 1, "one RTP timestamp for the tone");
    a.ok(r.pkts >= 5 && r.pkts <= 9, "about one packet per 50 ms plus 3 ends: " + r.pkts);
    a.eq(r.out, r.pkts, "every packet sent was received");
    closeAll(x, y);
});

t.test("RFC 4733 event codes: # -> 11, A -> 12, d -> 15, * -> 10; ',' sends no packet", async (a) => {
    const { x, y, xs, ry } = await pair();
    for (const [c, code] of [["#", 11], ["A", 12], ["d", 15], ["*", 10], ["0", 0], ["9", 9]]) {
        const r = await tones(a, xs, ry, c, 40, 80);
        a.eq(r.s.teEvent, code, JSON.stringify(c) + " -> " + code);
        a.eq(r.ends, 3);
        a.eq(r.s.teDur, 40 * 48);
    }
    const out0 = H.counts(xs[0].sender).dtmfOut, p0 = H.stats(ry[0].receiver).tePkts;
    const d = xs[0].sender.dtmf;
    d.insertDTMF(",", 40, 30);
    await delay(300);
    a.eq(H.counts(xs[0].sender).dtmfOut, out0, "',' is a pause: no packet");
    a.eq(H.stats(ry[0].receiver).tePkts, p0);
    d.insertDTMF("");
    closeAll(x, y);
});

t.test("RFC 4733 2.5.1.3: a tone longer than 0xFFFF units is split into segments (new timestamp, no new marker)", async (a) => {
    const { x, y, xs, ry } = await pair();
    const r = await tones(a, xs, ry, "5", 2000, 150);
    a.eq(r.s.teEvent, 5);
    a.eq(r.segs, 2, "96000 units = 65535 + 30465");
    a.eq(r.markers, 1);
    a.eq(r.s.teDur, 96000 - 65535, "the last segment carries the remainder");
    a.eq(r.ends, 3);
    closeAll(x, y);
});

async function relayChain(opts = {}) {
    const p1 = await pair();
    const fwd = p1.ry[0].receiver.track;
    const y2 = new RTCPeerConnection(LO), z = new RTCPeerConnection(LO);
    trickle(y2, z);
    const ys = y2.addTransceiver(fwd, { direction: "sendonly" });
    if (opts.noTe) ys.setCodecPreferences(RTCRtpSender.getCapabilities("audio").codecs.filter((c) => !/telephone-event/i.test(c.mimeType)));
    let zt = null;
    z.ontrack = (e) => { zt = e.transceiver; };
    await negotiate(y2, z);
    await Promise.all([waitState(y2, "connected"), waitState(z, "connected")]);
    return { ...p1, fwd, y2, z, ys, zt };
}

t.test("relay: a received track added to another pc is forwarded with that sender's SSRC; CSRCs and levels kept", async (a) => {
    const c = await relayChain();
    const zr = c.zt.receiver;
    let un = 0;
    zr.track.onunmute = () => un++;
    a.eq(H.send(c.xs[0].sender, { count: 4, ts: 1000, csrc: [77, 88], level: 20, clevels: [3, 40] }), 4);
    a.ok(await until(() => (H.stats(zr) || {}).pkts === 4), "z received the 4 relayed packets");
    a.ok(await until(() => un === 1), "z's track unmuted");
    a.eq(H.stats(zr).ssrc, ssrcOf(c.y2, c.ys.mid), "source is y2's sender SSRC, not x's");
    a.ok(H.stats(zr).ssrc !== ssrcOf(c.x, c.xs[0].mid));
    a.eq(H.stats(zr).lost, 0, "sequence numbers are contiguous after rewriting");
    const ss = zr.getSynchronizationSources()[0];
    a.near(ss.audioLevel, Math.pow(10, -20 / 20), 1e-9, "RFC 6464 byte kept");
    const cs = zr.getContributingSources().map((s) => s.source + ":" + s.audioLevel.toFixed(4)).sort();
    a.eq(cs.join(","), ["77:" + Math.pow(10, -3 / 20).toFixed(4), "88:" + Math.pow(10, -40 / 20).toFixed(4)].sort().join(","), "CSRCs and RFC 6465 levels kept");
    a.eq(H.counts(c.ys.sender).relayOut, 4);
    const ts0 = ss.rtpTimestamp;
    a.ok(ts0 !== c.ry[0].receiver.getSynchronizationSources()[0].rtpTimestamp, "timestamps rebased onto the sender's random offset");
    H.send(c.xs[0].sender, { count: 1, ts: 1000 + 4 * 960 });
    a.ok(await until(() => H.stats(zr).pkts === 5));
    a.eq((zr.getSynchronizationSources()[0].rtpTimestamp - ts0) >>> 0, 960, "timestamp deltas preserved (last of 4 at +3*960, this one at +4*960)");
    closeAll(c.x, c.y, c.y2, c.z);
});

t.test("relay: payload types the outgoing m= lacks are skipped (counted); DTMF relayed when negotiated", async (a) => {
    const c = await relayChain({ noTe: true });
    const zr = c.zt.receiver, tePt = ptOf(c.x, "telephone-event");
    a.eq(ptOf(c.y2, "telephone-event"), -1, "y2 -> z negotiated no telephone-event");
    H.send(c.xs[0].sender, { count: 2 });
    H.send(c.xs[0].sender, { count: 3, pt: tePt, bytes: 4 });
    a.ok(await until(() => H.counts(c.ys.sender).relaySkip === 3), "3 telephone-event packets skipped");
    a.ok(await until(() => (H.stats(zr) || {}).pkts === 2), "the 2 opus packets forwarded");
    await delay(50);
    a.eq(H.counts(zr).ptDrop, 0, "z saw no unknown payload type");
    closeAll(c.x, c.y, c.y2, c.z);
    const d = await relayChain();
    const dr = d.zt.receiver;
    await tones(a, d.xs, d.ry, "7", 60, 150);
    a.ok(await until(() => (H.stats(dr) || {}).teEnds === 3), "z got the three end packets through the relay");
    a.eq(H.stats(dr).teEvent, 7);
    a.eq(H.stats(dr).teDur, 60 * 48);
    closeAll(d.x, d.y, d.y2, d.z);
});

t.test("relay stops on replaceTrack(null) and when the relayed track ends", async (a) => {
    const c = await relayChain();
    const zr = c.zt.receiver;
    H.send(c.xs[0].sender, { count: 2 });
    a.ok(await until(() => (H.stats(zr) || {}).pkts === 2));
    await c.ys.sender.replaceTrack(null);
    H.send(c.xs[0].sender, { count: 2 });
    a.ok(await until(() => H.stats(c.ry[0].receiver).pkts === 4), "y still receives");
    await delay(100);
    a.eq(H.stats(zr).pkts, 2, "nothing relayed after replaceTrack(null)");
    await c.ys.sender.replaceTrack(c.fwd);
    H.send(c.xs[0].sender, { count: 1 });
    a.ok(await until(() => H.stats(zr).pkts === 3), "relaying again after replaceTrack(track)");
    a.eq(H.stats(zr).lost, 0, "the sender's sequence continues across the gap (x sent 2 that were not relayed)");
    c.fwd.stop();
    H.send(c.xs[0].sender, { count: 2 });
    a.ok(await until(() => H.stats(c.ry[0].receiver).pkts === 7));
    await delay(100);
    a.eq(H.stats(zr).pkts, 3, "nothing relayed once the relayed track ended");
    a.eq(H.send(c.ys.sender, { count: 1 }), 0, "harness: no packets from an ended track");
    c.xs[0].sender.track.stop();
    a.eq(H.send(c.xs[0].sender, { count: 1 }), 0, "harness: no packets from an ended local track");
    closeAll(c.x, c.y, c.y2, c.z);
});

t.test("RFC 4733: consecutive tones get distinct RTP timestamps (one event each)", async (a) => {
    const { x, y, xs, ry } = await pair();
    const r = await tones(a, xs, ry, "12", 40, 100);
    a.eq(r.segs, 2, "two tones, two timestamps");
    a.eq(r.markers, 2);
    a.eq(r.ends, 6);
    a.eq(r.s.teEvent, 2);
    closeAll(x, y);
});

t.test("relay: the payload type map follows the source's negotiation (sink added before the source has codecs)", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    const xs = x.addTransceiver(srcTrack("audio"), { direction: "sendrecv" });
    let fwd = null;
    y.ontrack = (e) => { fwd = e.track; };
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    await y.setRemoteDescription(o);
    a.ok(fwd !== null, "remote track exists after sRD, before y's answer");
    const y2 = new RTCPeerConnection(LO), z = new RTCPeerConnection(LO);
    trickle(y2, z);
    y2.addTransceiver(fwd, { direction: "sendonly" });
    let zt = null;
    z.ontrack = (e) => { zt = e.transceiver; };
    await negotiate(y2, z);
    await Promise.all([waitState(y2, "connected"), waitState(z, "connected")]);
    await y.setLocalDescription(await y.createAnswer());
    await x.setRemoteDescription(y.localDescription);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    H.send(xs.sender, { count: 3 });
    a.ok(await until(() => (H.stats(zt.receiver) || {}).pkts === 3), "z receives once the source negotiated its codecs");
    a.eq(H.counts(y2.getSenders()[0]).relaySkip, 0);
    closeAll(x, y, y2, z);
});

t.test("relay: a tone on the relaying sender takes the next sequence numbers; relaying continues without replay or loss", async (a) => {
    const c = await relayChain();
    const zr = c.zt.receiver;
    H.send(c.xs[0].sender, { count: 3 });
    a.ok(await until(() => (H.stats(zr) || {}).pkts === 3));
    const t0 = H.stats(zr).tePkts;
    const d = c.ys.sender.dtmf;
    a.eq(d.canInsertDTMF, true);
    await new Promise((res) => { d.ontonechange = (e) => { if (e.tone === "") res(); }; d.insertDTMF("4", 40, 30); });
    await delay(50);
    const tePkts = H.stats(zr).tePkts - t0;
    a.ok(tePkts >= 4, "tone packets reached z: " + tePkts);
    H.send(c.xs[0].sender, { count: 3 });
    a.ok(await until(() => H.stats(zr).pkts === 6 + tePkts), "relayed packets after the tone arrive");
    a.eq(H.counts(zr).srtpReplay, 0, "no sequence number reused");
    a.eq(H.stats(zr).lost, 0, "no gap");
    closeAll(c.x, c.y, c.y2, c.z);
});

t.test("relay: closing the destination pc first is safe; the source keeps receiving", async (a) => {
    const c = await relayChain();
    H.send(c.xs[0].sender, { count: 2 });
    a.ok(await until(() => (H.stats(c.zt.receiver) || {}).pkts === 2));
    c.y2.close();
    H.send(c.xs[0].sender, { count: 3 });
    a.ok(await until(() => H.stats(c.ry[0].receiver).pkts === 5), "y still receives after y2 closed");
    a.eq(c.fwd.readyState, "live");
    closeAll(c.x, c.y, c.z);
    H.send(c.xs[0].sender, { count: 1 });
    await delay(20);
});

await t.run();
