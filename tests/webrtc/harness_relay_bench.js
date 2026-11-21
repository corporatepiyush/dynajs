// flags: --std (run under tests/webrtc/webrtc_media_harness: ./obj/release/src/webrtc_media_harness tests/webrtc/harness_relay_bench.js)
// covers: bench dyna:webrtc.RTCRtpSender -- W6b rule 11: relay of a received track fanned out to N senders (N = 1, 4,
// 16, 48 m= sections on one pc; the offer list cap is 64 m= sections): CPU per forwarded packet (receive + forward to
// every sink + the far end's receive, all in one process), media-state bytes per sink on the relaying pc, peak RSS
// over repeated rounds; plus RFC 4733 tone packets per tone.
import { RTCPeerConnection } from "dyna:webrtc";
import { LO, trickle, negotiate, waitState, delay } from "./rtc_util.js";

const H = globalThis.rtpHarness;
const NS = (scriptArgs.find((s) => s.startsWith("--n=")) || "--n=1,4,16,48").slice(4).split(",").map(Number);
const ROUNDS = Number((scriptArgs.find((s) => s.startsWith("--rounds=")) || "--rounds=3").slice(9));
const PER = Number((scriptArgs.find((s) => s.startsWith("--per=")) || "--per=2000").slice(6));
const BYTES = 160;

async function connect(x, y) {
    trickle(x, y);
    await negotiate(x, y);
    await Promise.all([waitState(x, "connected", 20000), waitState(y, "connected", 20000)]);
}

async function round(n) {
    const keep = new RTCPeerConnection(LO);
    const src = keep.addTransceiver("audio").receiver.track;
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    const xs = x.addTransceiver(src, { direction: "sendonly" });
    await connect(x, y);
    const ry = y.getTransceivers()[0].receiver;
    const y2 = new RTCPeerConnection(LO), z = new RTCPeerConnection(LO);
    y2.addTransceiver("audio", { direction: "sendonly" });
    await connect(y2, z);
    const bytes0 = H.bytes(y2.getSenders()[0]);
    y2.getTransceivers()[0].stop();
    const ys = [];
    for (let i = 0; i < n; i++) ys.push(y2.addTransceiver(ry.track, { direction: "sendonly" }));
    await negotiate(y2, z);
    await delay(20);
    const zr = ys.map((t) => z.getTransceivers().find((u) => u.mid === t.mid).receiver);
    const bytesSinks = H.bytes(ys[0].sender) - bytes0 + H.relayBytes(ry.track);
    const base = H.counts(zr[0]).rtpIn, out0 = H.counts(ys[0].sender).relayOut;
    const got = () => H.counts(zr[0]).rtpIn - base;
    const batch = Math.max(4, Math.floor(256 / n));
    let sent = 0, ts = 0, us = 0;
    while (sent < PER) {
        const c0 = H.cpu();
        let k = 0;
        for (; k < batch && sent + k < PER; k++) H.send(xs.sender, { count: 1, ts: ts += 960, bytes: BYTES, level: 30 });
        sent += k;
        const deadline = Date.now() + 5000;
        while (got() < sent * n && Date.now() < deadline) await delay(1);
        us += H.cpu() - c0;
    }
    const fwd = H.counts(ys[0].sender).relayOut - out0, lost = PER * n - got();
    const d = xs.sender.dtmf, o0 = H.counts(xs.sender).dtmfOut;
    await new Promise((res) => { d.ontonechange = (e) => { if (e.tone === "") res(); }; d.insertDTMF("123", 100, 50); });
    await delay(50);
    const dtmfPkts = (H.counts(xs.sender).dtmfOut - o0) / 3;
    for (const p of [x, y, y2, z, keep]) p.close();
    return { n, fwd, lost, nsPerOut: (us * 1000) / (PER * n), bytesPerSink: bytesSinks / n, dtmfPkts };
}

console.log("N_sinks\tround\tforwarded\tlost\tns/forwarded_pkt\trelay_bytes/sink\tdtmf_pkts/tone\tpeak_rss_MB");
for (const n of NS) {
    for (let r = 1; r <= ROUNDS; r++) {
        const o = await round(n);
        console.log([n, r, o.fwd, o.lost, o.nsPerOut.toFixed(0), o.bytesPerSink.toFixed(0), o.dtmfPkts.toFixed(1), (H.rss() / 1048576).toFixed(1)].join("\t"));
    }
}
