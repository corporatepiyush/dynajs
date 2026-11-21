// flags: --std
// bench_webrtc.js [section ...] -- dyna:webrtc end to end over loopback UDP, two RTCPeerConnections in one process:
//   size     data-channel throughput (MB/s, msgs/s) for message sizes 16 B .. 1 MiB, sender kept under a 4 MiB
//            bufferedAmount window, with peak RSS after each size
//   latency  64-byte ping-pong round trips: p50 / p99 / max (us)
//   channels peak-RSS growth per open data channel for 1 .. 1024 channels on one connection
//   pcs      peak-RSS growth per connected pair (1, 4, 16, 32 pairs)
//   cycles   connect / one message / close cycles: peak RSS every 50 cycles (plateau = no leak)
//   manych   256 open channels all carrying traffic: 256 B messages round-robin, MB/s and us per message
//   loss     16 KiB messages through a seeded lossy UDP relay (0%, 1%, 5% loss): MB/s, relay drops
//   burst    64 x 64 B messages sent in one turn and drained by the peer as one UDP batch: us per burst, per message
// No section = all. Every line prints the 1-minute load average next to it. BENCH_SMOKE_MS bounds a smoke run.
import * as sys from "dyna:sys";
import { RTCPeerConnection } from "dyna:webrtc";
import { LossyRelay, trickle, negotiate } from "./rtc_util.js";

const LO = { iceLocalAddresses: ["127.0.0.1"] };
const SMOKE = Number(sys.getEnv("BENCH_SMOKE_MS") || 0) > 0;
const want = new Set(scriptArgs.slice(1));
const on = (s) => want.size === 0 || want.has(s);
const rss = () => sys.memoryUsage().peakRss;
const load = () => { try { return String(sys.Exec("/usr/sbin/sysctl", ["-n", "vm.loadavg"]).stdout || "").trim().split(/\s+/)[1] || "?"; } catch (e) { return "?"; } };
const now = () => performance.now();
const MiB = 1048576;

function once(t, type) { return new Promise((r) => t.addEventListener(type, r, { once: true })); }

async function pair(nChannels = 1, init) {
    const a = new RTCPeerConnection(LO), b = new RTCPeerConnection(LO);
    a.onicecandidate = (e) => b.addIceCandidate(e.candidate).catch(() => {});
    b.onicecandidate = (e) => a.addIceCandidate(e.candidate).catch(() => {});
    const remote = [];
    let need = nChannels;
    const allRemote = new Promise((res) => { b.ondatachannel = (e) => { remote.push(e.channel); if (--need === 0) res(); }; });
    const local = [];
    for (let i = 0; i < nChannels; i++) local.push(a.createDataChannel("c" + i, init));
    const o = await a.createOffer();
    await a.setLocalDescription(o);
    await b.setRemoteDescription(o);
    const ans = await b.createAnswer();
    await b.setLocalDescription(ans);
    await a.setRemoteDescription(ans);
    await Promise.all(local.map((c) => (c.readyState === "open" ? null : once(c, "open"))));
    if (nChannels) await allRemote;
    return { a, b, local, remote };
}

async function throughput(size, total) {
    const p = await pair();
    const tx = p.local[0], rx = p.remote[0];
    rx.binaryType = "arraybuffer";
    const n = Math.max(4, Math.floor(total / size));
    const msg = new Uint8Array(size);
    let got = 0;
    const done = new Promise((res) => { rx.onmessage = () => { if (++got === n) res(); }; });
    tx.bufferedAmountLowThreshold = 2 * MiB;
    const t0 = now();
    let sent = 0;
    while (sent < n) {
        while (sent < n && tx.bufferedAmount < 4 * MiB) { tx.send(msg); sent++; }
        if (sent < n) await once(tx, "bufferedamountlow");
    }
    await done;
    const ms = now() - t0;
    p.a.close();
    p.b.close();
    return { n, ms, mbs: (n * size) / MiB / (ms / 1000), mps: n / (ms / 1000) };
}

async function latency(rounds) {
    const p = await pair();
    const tx = p.local[0], rx = p.remote[0];
    rx.onmessage = (e) => rx.send(e.data);
    const msg = "x".repeat(64);
    const out = [];
    for (let i = 0; i < rounds; i++) {
        const t0 = now();
        const r = once(tx, "message");
        tx.send(msg);
        await r;
        out.push((now() - t0) * 1000);
    }
    p.a.close();
    p.b.close();
    out.sort((x, y) => x - y);
    return { p50: out[Math.floor(out.length * 0.5)], p99: out[Math.floor(out.length * 0.99)], max: out[out.length - 1] };
}

if (on("size")) {
    console.log("# size sweep: message size, count, MB/s, msgs/s, peak RSS (MiB), load");
    for (const size of SMOKE ? [16, 1024] : [16, 256, 1024, 16384, 65536, 262144, MiB]) {
        const total = SMOKE ? 256 * 1024 : size < 1024 ? 4 * MiB : 64 * MiB;
        const r = await throughput(size, total);
        console.log(`size ${String(size).padStart(8)} B  n ${String(r.n).padStart(7)}  ${r.mbs.toFixed(1).padStart(8)} MB/s  ${Math.round(r.mps).toString().padStart(8)} msg/s  rss ${(rss() / MiB).toFixed(1)}  load ${load()}`);
    }
}

if (on("latency")) {
    const r = await latency(SMOKE ? 50 : 5000);
    console.log(`latency 64 B ping-pong  p50 ${r.p50.toFixed(1)} us  p99 ${r.p99.toFixed(1)} us  max ${r.max.toFixed(1)} us  load ${load()}`);
}

if (on("channels")) {
    console.log("# channels: count, peak RSS (MiB), bytes per channel over the 1-channel pair, load");
    let base = 0;
    for (const k of SMOKE ? [1, 16] : [1, 16, 64, 256, 1024]) {
        const p = await pair(k);
        for (const c of p.local) c.send("hi");
        await new Promise((r) => setTimeout(r, 50));
        const r = rss();
        if (k === 1) base = r;
        console.log(`channels ${String(k).padStart(5)}  rss ${(r / MiB).toFixed(1)}  per-channel ${k > 1 ? ((r - base) / (k - 1)).toFixed(0) : "-"} B  load ${load()}`);
        p.a.close();
        p.b.close();
    }
}

if (on("pcs")) {
    console.log("# pcs: connected pairs alive at once, peak RSS (MiB), bytes per pair over the previous step, load");
    let prev = rss(), prevN = 0;
    const live = [];
    for (const n of SMOKE ? [1, 2] : [1, 4, 16, 32]) {
        while (live.length < n) live.push(await pair());
        const r = rss();
        console.log(`pairs ${String(n).padStart(4)}  rss ${(r / MiB).toFixed(1)}  per-pair ${((r - prev) / (n - prevN)).toFixed(0)} B  load ${load()}`);
        prev = r;
        prevN = n;
    }
    for (const p of live) { p.a.close(); p.b.close(); }
}

if (on("cycles")) {
    console.log("# cycles: connect + 1 message + close, peak RSS every 50 cycles (MiB)");
    const N = SMOKE ? 10 : 400;
    const t0 = now();
    for (let i = 1; i <= N; i++) {
        const p = await pair();
        const got = once(p.remote[0], "message");
        p.local[0].send("ping");
        await got;
        p.a.close();
        p.b.close();
        if (i % 50 === 0 || i === N) console.log(`cycle ${String(i).padStart(4)}  rss ${(rss() / MiB).toFixed(2)}  ${((now() - t0) / i).toFixed(2)} ms/cycle  load ${load()}`);
    }
}

if (on("manych")) {
    console.log("# manych: 256 channels, 256 B messages round-robin over all of them, MB/s, us per message, peak RSS (MiB)");
    const K = 256, per = SMOKE ? 4 : 64;
    const p = await pair(K);
    const msg = new Uint8Array(256);
    let got = 0;
    const n = K * per;
    const done = new Promise((res) => { for (const c of p.remote) c.onmessage = () => { if (++got === n) res(); }; });
    const t0 = now();
    for (let r = 0; r < per; r++) {
        for (const c of p.local) c.send(msg);
        if (p.local[0].bufferedAmount > 2 * MiB) await new Promise((res) => setTimeout(res, 0));
    }
    await done;
    const ms = now() - t0;
    console.log(`manych ${K} channels  n ${n}  ${((n * 256) / MiB / (ms / 1000)).toFixed(1)} MB/s  ${((ms * 1000) / n).toFixed(2)} us/msg  rss ${(rss() / MiB).toFixed(1)}  load ${load()}`);
    p.a.close();
    p.b.close();
}

async function lossyThroughput(loss, total) {
    const relay = new LossyRelay({ loss, seed: 7 });
    const a = new RTCPeerConnection(LO), b = new RTCPeerConnection(LO);
    relay.pair(a, b);
    trickle(a, b, relay.rewrite());
    const tx = a.createDataChannel("loss");
    const rxp = new Promise((res) => { b.ondatachannel = (e) => res(e.channel); });
    await negotiate(a, b);
    if (tx.readyState !== "open") await once(tx, "open");
    const rx = await rxp;
    const size = 16384, n = Math.max(8, Math.floor(total / size));
    const msg = new Uint8Array(size);
    let got = 0;
    const done = new Promise((res) => { rx.onmessage = () => { if (++got === n) res(); }; });
    tx.bufferedAmountLowThreshold = MiB;
    const t0 = now();
    let sent = 0;
    while (sent < n) {
        while (sent < n && tx.bufferedAmount < 2 * MiB) { tx.send(msg); sent++; }
        if (sent < n) await once(tx, "bufferedamountlow");
    }
    await done;
    const ms = now() - t0;
    a.close();
    b.close();
    relay.close();
    return { mbs: (n * size) / MiB / (ms / 1000), dropped: relay.stats.dropped, fwd: relay.stats.fwd };
}

if (on("loss")) {
    console.log("# loss: 16 KiB messages through a JS UDP relay; MB/s, relay forwarded/dropped datagrams, load");
    for (const loss of SMOKE ? [0, 0.05] : [0, 0.01, 0.05]) {
        const r = await lossyThroughput(loss, SMOKE ? 256 * 1024 : 8 * MiB);
        console.log(`loss ${String(loss * 100).padStart(3)}%  ${r.mbs.toFixed(1).padStart(7)} MB/s  fwd ${r.fwd}  dropped ${r.dropped}  load ${load()}`);
    }
}

if (on("burst")) {
    console.log("# burst: 64 x 64 B sent in one turn, peer echoes a count when all 64 arrived; us per burst / per message");
    const p = await pair();
    const tx = p.local[0], rx = p.remote[0];
    const rounds = SMOKE ? 20 : 2000;
    let got = 0;
    rx.onmessage = () => { if (++got === 64) { got = 0; rx.send("k"); } };
    const msg = new Uint8Array(64);
    const out = [];
    for (let i = 0; i < rounds; i++) {
        const ack = once(tx, "message");
        const t0 = now();
        for (let j = 0; j < 64; j++) tx.send(msg);
        await ack;
        out.push((now() - t0) * 1000);
    }
    out.sort((x, y) => x - y);
    const p50 = out[Math.floor(out.length / 2)];
    console.log(`burst 64x64B  p50 ${p50.toFixed(1)} us/burst  ${(p50 / 64).toFixed(2)} us/msg  p99 ${out[Math.floor(out.length * 0.99)].toFixed(1)} us  load ${load()}`);
    p.a.close();
    p.b.close();
}
