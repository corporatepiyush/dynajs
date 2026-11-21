// flags: --std (run under tests/webrtc/webrtc_media_harness: ./obj/release/src/webrtc_media_harness tests/webrtc/harness_media_bench.js)
// covers: bench dyna:webrtc.RTCRtpReceiver -- W6 rule 11: CPU per packet (send: RTP build + SRTP protect + sendto; receive:
// recvmsg + demux + SRTP unprotect + routing + stats + sources) and memory (media-state bytes per stream, peak RSS)
// across stream counts N = 1, 4, 16, 64 (64 = WRT_SRTP_MAX_STREAMS per direction), repeated rounds for growth.
// Packets go in batches that the loopback socket buffer holds, and each batch is drained before the next.
import { RTCPeerConnection } from "dyna:webrtc";
import { LO, trickle, negotiate, waitState, delay } from "./rtc_util.js";

const H = globalThis.rtpHarness;
const NS = (scriptArgs.find((s) => s.startsWith("--n=")) || "--n=1,4,16,64").slice(4).split(",").map(Number);
const ROUNDS = Number((scriptArgs.find((s) => s.startsWith("--rounds=")) || "--rounds=3").slice(9));
const PER = Number((scriptArgs.find((s) => s.startsWith("--per=")) || "--per=2000").slice(6));
const BYTES = 160;

const held = [];
function srcTrack() { const pc = new RTCPeerConnection(LO); held.push(pc); return pc.addTransceiver("audio").receiver.track; }

async function round(n) {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    const xs = [];
    for (let i = 0; i < n; i++) xs.push(x.addTransceiver(srcTrack(), { direction: "sendonly" }));
    await negotiate(x, y);
    await Promise.all([waitState(x, "connected", 20000), waitState(y, "connected", 20000)]);
    const rs = xs.map((tr) => y.getTransceivers().find((u) => u.mid === tr.mid).receiver);
    const total = n * PER, batch = 256;
    let sent = 0, sendUs = 0, recvUs = 0, ts = 0;
    const base = H.counts(rs[0]).rtpIn;
    const got = () => H.counts(rs[0]).rtpIn - base;
    while (sent < total) {
        const c0 = H.cpu();
        let k = 0;
        for (; k < batch && sent + k < total; k++) H.send(xs[(sent + k) % n].sender, { count: 1, ts: ts += 960, bytes: BYTES, level: 30 });
        const c1 = H.cpu();
        sent += k;
        const deadline = Date.now() + 5000;
        while (got() < sent && Date.now() < deadline) await delay(1);
        sendUs += c1 - c0;
        recvUs += H.cpu() - c1;
    }
    const rx = got();
    const bytesX = H.bytes(xs[0].sender), bytesY = H.bytes(rs[0]);
    x.close(); y.close();
    for (const p of held.splice(0)) p.close();
    return { n, rx, lost: total - rx, sendNs: (sendUs * 1000) / total, recvNs: (recvUs * 1000) / total, bytesX, bytesY };
}

console.log("N\tround\tpkts\tlost\tsend_ns/pkt\trecv_ns/pkt\ttx_bytes/stream\trx_bytes/stream\tpeak_rss_MB");
for (const n of NS) {
    for (let r = 1; r <= ROUNDS; r++) {
        const o = await round(n);
        console.log([n, r, o.rx, o.lost, o.sendNs.toFixed(0), o.recvNs.toFixed(0), (o.bytesX / n).toFixed(0), (o.bytesY / n).toFixed(0), (H.rss() / 1048576).toFixed(1)].join("\t"));
    }
}
