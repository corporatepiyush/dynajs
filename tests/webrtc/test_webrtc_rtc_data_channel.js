// covers: dyna:webrtc.RTCDataChannel -- two RTCPeerConnections in one process over real UDP sockets: open, ordered and
// unordered delivery, the four DCEP payload kinds, large messages, many channels, id parity, negotiated channels,
// close in both directions, bufferedAmount/bufferedamountlow, partial reliability, lossy/duplicating/reordering links.
// Event dispatch (R3-3): every event fired from a received batch is its own task, so microtasks queued by one
// onmessage run before the next message event (HTML event loop microtask checkpoint); four sends bundle into one
// SCTP packet, so without the checkpoint the order would be m1 m2 m3 m4 u1 u2 u3 u4.
// Per-batch drain (R3-2): datagrams from one socket wakeup are processed first and the events drained once (udpb
// batch_end), so a 300-message burst shows transport receiveWakeups below packetsReceived.
// Both ends send INIT once DTLS connects (R1-M8): sctp-transport initChunksSent >= 1 on both sides.
// R2-M3: close() plus dropping the last reference inside onmessage, or in a microtask queued there, is memory-safe
// (run under ASan via .obj-san/dynajs).
// Partial reliability: the 30% loss runs only while the maxRetransmits:0 burst creates abandoned TSNs; the reliable phase then
// runs clean. RFC 3758 retransmits FORWARD-TSN only on T3 and advances only over abandoned TSNs (C2), so under sustained 30%
// two-way loss each lost FORWARD-TSN or SACK costs a doubled RTO: seed 21 took 1+2+4+8+16 = 31 s, and one more loss took 63 s.
import { RTCPeerConnection, RTCDataChannel } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { hostScale } from "../lib/hostscale.js";
import { LO, connectPair, waitEvent, closeAll, delay, LossyRelay, trickle, negotiate, waitState, lcg } from "./rtc_util.js";

const t = suite("webrtc_datachannel");

function collect(ch, n, ms = 20000) {
    return new Promise((res, rej) => {
        const out = [];
        const tm = setTimeout(() => rej(new Error("only " + out.length + "/" + n + " messages")), ms);
        ch.addEventListener("message", function h(e) {
            out.push(e.data);
            if (out.length === n) { clearTimeout(tm); ch.removeEventListener("message", h); res(out); }
        });
    });
}

function bytes(n, seed) {
    const u = new Uint8Array(n), r = lcg(seed);
    for (let i = 0; i < n; i++) u[i] = (r() * 256) | 0;
    return u;
}

function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

t.test("open: both ends open, label/protocol/ordered survive DCEP, id parity follows the DTLS role", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair({ label: "chat", init: { protocol: "p1" } });
    a.ok(dc instanceof RTCDataChannel);
    a.eq(dc.readyState, "open");
    a.eq(rdc.readyState, "open");
    a.eq(rdc.label, "chat");
    a.eq(rdc.protocol, "p1");
    a.eq(rdc.ordered, true);
    a.eq(rdc.negotiated, false);
    a.eq(rdc.maxRetransmits, null);
    a.eq(rdc.maxPacketLifeTime, null);
    a.eq(dc.id, rdc.id);
    a.eq(dc.id % 2, 1, "offerer is the DTLS server (answerer chose setup:active) so its ids are odd (RFC 8832 6)");
    a.eq(pa.sctp.state, "connected");
    a.ok(pa.sctp.maxMessageSize >= 65536);
    closeAll(pa, pb);
    a.eq(dc.readyState, "closed");
    a.eq(pa.connectionState, "closed");
}, { timeoutMs: 20000 });

t.test("payload kinds: string, unicode, empty string, binary, empty binary, typed-array views, Blob", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair();
    const got = collect(rdc, 7);
    dc.send("hello");
    dc.send("héllo \u{1F600} 世界");
    dc.send("");
    dc.send(new Uint8Array([1, 2, 3]).buffer);
    dc.send(new ArrayBuffer(0));
    dc.send(new Uint16Array([0x0102, 0x0304]).subarray(1));
    dc.send(new Blob([new Uint8Array([9, 8, 7])]));
    const m = await got;
    a.eq(m[0], "hello");
    a.eq(m[1], "héllo \u{1F600} 世界");
    a.eq(m[2], "");
    a.ok(m[3] instanceof ArrayBuffer);
    a.deep(Array.from(new Uint8Array(m[3])), [1, 2, 3]);
    a.ok(m[4] instanceof ArrayBuffer && m[4].byteLength === 0, "empty binary arrives as an empty ArrayBuffer (PPID 57)");
    a.eq(new Uint8Array(m[5]).length, 2);
    a.deep(Array.from(new Uint8Array(m[6])), [9, 8, 7]);
    rdc.binaryType = "blob";
    a.eq(rdc.binaryType, "blob");
    rdc.binaryType = "bogus";
    a.eq(rdc.binaryType, "blob", "an invalid BinaryType is ignored");
    const g2 = collect(rdc, 1);
    dc.send(new Uint8Array([5]));
    const [blob] = await g2;
    a.ok(blob instanceof Blob);
    a.eq(blob.size, 1);
    closeAll(pa, pb);
}, { timeoutMs: 20000 });

t.test("a microtask checkpoint runs between message events from one packet (R3-3)", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair();
    const order = [];
    const done = new Promise((res) => {
        rdc.onmessage = (e) => {
            order.push("m" + e.data);
            queueMicrotask(() => { order.push("u" + e.data); if (e.data === "4") res(); });
        };
    });
    dc.send("1"); dc.send("2"); dc.send("3"); dc.send("4");
    await done;
    a.eq(order.join(" "), "m1 u1 m2 u2 m3 u3 m4 u4");
    closeAll(pa, pb);
});

t.test("one drain per socket wakeup: a burst is processed in batches (R3-2), and both ends sent INIT (R1-M8)", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair();
    const N = 300, payload = "x".repeat(1100);
    let got = 0;
    const done = new Promise((res) => { rdc.onmessage = () => { if (++got === N) res(); }; });
    for (let i = 0; i < N; i++) dc.send(payload);
    await done;
    const pick = async (pc, type) => { for (const s of (await pc.getStats()).values()) if (s.type === type) return s; return null; };
    const tr = await pick(pb, "transport");
    a.ok(tr.receiveWakeups > 0 && tr.receiveWakeups < tr.packetsReceived, "wakeups " + tr.receiveWakeups + " < packets " + tr.packetsReceived);
    const sa = await pick(pa, "sctp-transport"), sb = await pick(pb, "sctp-transport");
    a.ok(sa.initChunksSent >= 1 && sb.initChunksSent >= 1, "INIT sent by both ends: " + sa.initChunksSent + " " + sb.initChunksSent);
    closeAll(pa, pb);
});

t.test("close() and dropping the last reference inside onmessage or a microtask queued there is safe (R2-M3)", async (a) => {
    for (const mode of ["handler", "microtask"]) {
        let { a: pa, b: pb, dc, rdc } = await connectPair();
        const got = new Promise((res) => {
            rdc.onmessage = () => {
                const f = () => { if (pb) { pb.close(); pb = null; rdc = null; res(); } };
                if (mode === "handler") f(); else queueMicrotask(f);
            };
        });
        dc.send("x"); dc.send("y"); dc.send("z");
        await got;
        await delay(100);
        a.ok(pb === null, mode);
        pa.close();
        await delay(50);
    }
});

t.test("ordered: 2000 messages arrive complete and in order both ways", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair();
    const n = 2000;
    const g1 = collect(rdc, n), g2 = collect(dc, n);
    for (let i = 0; i < n; i++) { dc.send("m" + i); rdc.send("r" + i); }
    const [x, y] = await Promise.all([g1, g2]);
    let ok1 = true, ok2 = true;
    for (let i = 0; i < n; i++) { if (x[i] !== "m" + i) ok1 = false; if (y[i] !== "r" + i) ok2 = false; }
    a.ok(ok1, "a->b in order");
    a.ok(ok2, "b->a in order");
    closeAll(pa, pb);
}, { timeoutMs: 30000 });

t.test("large: 256 KiB, 1 MiB and 4 MiB binary messages and a 300 KiB string reassemble byte-exact", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair();
    const sizes = [256 * 1024, 1 << 20, 4 << 20];
    const bufs = sizes.map((s, i) => bytes(s, i + 7));
    const str = "x".repeat(300 * 1024 - 3) + "€";
    const got = collect(rdc, 4, 60000);
    for (const b of bufs) dc.send(b);
    dc.send(str);
    const m = await got;
    for (let i = 0; i < 3; i++) a.ok(sameBytes(new Uint8Array(m[i]), bufs[i]), "message " + sizes[i] + " bytes intact");
    a.eq(m[3], str);
    a.throws(() => dc.send(new Uint8Array(pa.sctp.maxMessageSize + 1)), TypeError);
    closeAll(pa, pb);
}, { timeoutMs: 90000 });

t.test("many channels: 128 channels each echo one message, ids distinct and odd", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    const first = x.createDataChannel("c0");
    y.ondatachannel = (e) => { e.channel.onmessage = (m) => e.channel.send("echo:" + m.data); };
    await negotiate(x, y);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    await waitEvent(first, "open");
    const N = 128, chans = [first];
    for (let i = 1; i < N; i++) chans.push(x.createDataChannel("c" + i));
    await Promise.all(chans.map((c) => c.readyState === "open" ? null : waitEvent(c, "open")));
    const ids = new Set(chans.map((c) => c.id));
    a.eq(ids.size, N);
    a.ok(chans.every((c) => c.id % 2 === 1));
    const replies = await Promise.all(chans.map((c, i) => { const p = waitEvent(c, "message"); c.send("c" + i); return p; }));
    a.ok(replies.every((e, i) => e.data === "echo:c" + i));
    closeAll(x, y);
}, { timeoutMs: 60000 });

t.test("negotiated: both sides create id 4 out of band, no DCEP, messages flow", async (a) => {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    let announced = 0;
    y.ondatachannel = () => announced++;
    const cx = x.createDataChannel("n", { negotiated: true, id: 4 });
    const cy = y.createDataChannel("n", { negotiated: true, id: 4 });
    a.eq(cx.id, 4);
    a.eq(cx.negotiated, true);
    await negotiate(x, y);
    await Promise.all([waitEvent(cx, "open"), waitEvent(cy, "open")]);
    const p = waitEvent(cy, "message");
    cx.send("neg");
    a.eq((await p).data, "neg");
    a.eq(announced, 0, "no datachannel event for a negotiated channel");
    closeAll(x, y);
}, { timeoutMs: 20000 });

t.test("close: local close() -> closing then close on both ends; the stream id is reusable", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair();
    const ev = [];
    rdc.onclosing = () => ev.push("r-closing");
    rdc.onclose = () => ev.push("r-close");
    const lc = waitEvent(dc, "close");
    const rc = waitEvent(rdc, "close");
    dc.close();
    a.eq(dc.readyState, "closing");
    await Promise.all([lc, rc]);
    a.eq(dc.readyState, "closed");
    a.eq(rdc.readyState, "closed");
    a.deep(ev, ["r-closing", "r-close"]);
    a.throws(() => dc.send("x"), { type: DOMException, code: 11 });
    const again = pa.createDataChannel("again");
    const remote = waitEvent(pb, "datachannel");
    await waitEvent(again, "open");
    const r2 = (await remote).channel;
    a.eq(again.id, r2.id);
    closeAll(pa, pb);
}, { timeoutMs: 20000 });

t.test("close: remote side closes, messages queued before close are still delivered", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair();
    const got = collect(dc, 50);
    for (let i = 0; i < 50; i++) rdc.send("q" + i);
    rdc.close();
    const m = await got;
    a.eq(m.length, 50);
    a.eq(m[49], "q49");
    if (dc.readyState !== "closed") await waitEvent(dc, "close");
    a.eq(dc.readyState, "closed");
    closeAll(pa, pb);
}, { timeoutMs: 20000 });

t.test("bufferedAmount rises on send and bufferedamountlow fires once it drains below the threshold", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair();
    dc.bufferedAmountLowThreshold = 1024;
    a.eq(dc.bufferedAmountLowThreshold, 1024);
    const low = waitEvent(dc, "bufferedamountlow");
    const blk = new Uint8Array(64 * 1024);
    for (let i = 0; i < 16; i++) dc.send(blk);
    a.eq(dc.bufferedAmount, 16 * 64 * 1024, "bufferedAmount counts queued bytes synchronously");
    const got = collect(rdc, 16, 30000);
    await low;
    a.ok(dc.bufferedAmount <= 1024);
    await got;
    closeAll(pa, pb);
}, { timeoutMs: 40000 });

t.test("unreliable unordered channel (maxRetransmits 0) delivers everything on a clean link", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair({ init: { ordered: false, maxRetransmits: 0 } });
    a.eq(rdc.ordered, false);
    a.eq(rdc.maxRetransmits, 0);
    const got = collect(rdc, 500);
    for (let i = 0; i < 500; i++) dc.send("u" + i);
    const m = await got;
    a.eq(new Set(m).size, 500);
    closeAll(pa, pb);
}, { timeoutMs: 20000 });

t.test("maxPacketLifeTime survives DCEP", async (a) => {
    const { a: pa, b: pb, rdc } = await connectPair({ init: { ordered: false, maxPacketLifeTime: 150 } });
    a.eq(rdc.maxPacketLifeTime, 150);
    a.eq(rdc.maxRetransmits, null);
    closeAll(pa, pb);
}, { timeoutMs: 20000 });

for (const [name, cfg] of [["10% loss", { loss: 0.1, seed: 3 }], ["5% dup + 20% reorder", { dup: 0.05, reorder: 0.2, seed: 5 }],
    ["8% loss + dup + reorder", { loss: 0.08, dup: 0.03, reorder: 0.1, seed: 11 }]]) {
    t.test("lossy link (" + name + "): reliable ordered stream of 300 messages + one 512 KiB message arrive intact", async (a) => {
        const relay = new LossyRelay(cfg);
        const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
        relay.pair(x, y);
        trickle(x, y, relay.rewrite());
        const c = x.createDataChannel("lossy");
        const rp = waitEvent(y, "datachannel", 30000);
        await negotiate(x, y);
        const rc = (await rp).channel;
        if (c.readyState !== "open") await waitEvent(c, "open", 30000);
        const big = bytes(512 * 1024, 99);
        const got = collect(rc, 301, 60000);
        for (let i = 0; i < 300; i++) c.send("L" + i);
        c.send(big);
        const m = await got;
        let inOrder = true;
        for (let i = 0; i < 300; i++) if (m[i] !== "L" + i) inOrder = false;
        a.ok(inOrder, "ordered under " + name);
        a.ok(sameBytes(new Uint8Array(m[300]), big), "512 KiB intact");
        a.ok(relay.stats.dropped + relay.stats.duped + relay.stats.reordered > 0, "the relay actually disturbed traffic");
        closeAll(x, y);
        relay.close();
    }, { timeoutMs: 90000 });
}

t.test("partial reliability: set up clean, then 30% loss; maxRetransmits 0 unordered loses some, never stalls, a reliable channel still completes", async (a) => {
    const relay = new LossyRelay({ loss: 0, seed: 21 });
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    relay.pair(x, y);
    trickle(x, y, relay.rewrite());
    const rel = x.createDataChannel("rel");
    const unrel = x.createDataChannel("unrel", { ordered: false, maxRetransmits: 0 });
    const chans = {};
    y.ondatachannel = (e) => { chans[e.channel.label] = e.channel; };
    await negotiate(x, y);
    await Promise.all([waitEvent(rel, "open", 40000), unrel.readyState === "open" ? 0 : waitEvent(unrel, "open", 40000)]);
    while (!chans.unrel || !chans.rel) await delay(10);
    relay.loss = 0.3;
    let n = 0;
    chans.unrel.onmessage = () => n++;
    for (let i = 0; i < 400; i++) unrel.send("p" + i);
    await delay(1500);
    a.ok(n > 0 && n < 400, "some but not all unreliable messages arrived (" + n + ")");
    relay.loss = 0;
    const t0 = performance.now();
    const p = collect(chans.rel, 20, 40000);
    for (let i = 0; i < 20; i++) rel.send("after" + i);
    const m = await p;
    a.eq(m[19], "after19", "the association did not stall behind abandoned data (FORWARD-TSN)");
    // wall-clock: a deadline against a stall; the recovery waits on the backed-off T3-rtx timer (RFC 9260 6.3.3), which runs in wall time.
    a.ok(performance.now() - t0 < 20000 * hostScale(), "FORWARD-TSN recovery within the backed-off RTO (" + Math.round(performance.now() - t0) + " ms)"); // not perf: a stall deadline on a protocol timer
    closeAll(x, y);
    relay.close();
}, { timeoutMs: 90000 });

t.test("pc.close(): channels become closed without events, operations reject InvalidStateError", async (a) => {
    const { a: pa, b: pb, dc, rdc } = await connectPair();
    let fired = 0;
    dc.onclose = () => fired++;
    pa.close();
    a.eq(dc.readyState, "closed");
    a.eq(pa.signalingState, "closed");
    a.eq(pa.iceConnectionState, "closed");
    await a.rejects(pa.createOffer(), { type: DOMException, code: 11 });
    a.throws(() => pa.createDataChannel("z"), { type: DOMException, code: 11 });
    if (rdc.readyState !== "closed") await waitEvent(rdc, "close", 40000);
    a.eq(rdc.readyState, "closed", "the remote end learns of the abort");
    a.eq(fired, 0, "close() fires no close event on the local channels");
    closeAll(pb);
}, { timeoutMs: 60000 });

t.test("createDataChannel argument validation", async (a) => {
    const pc = new RTCPeerConnection();
    a.throws(() => pc.createDataChannel(), TypeError);
    a.throws(() => pc.createDataChannel("x", { maxRetransmits: 1, maxPacketLifeTime: 1 }), TypeError);
    a.throws(() => pc.createDataChannel("x", { negotiated: true }), TypeError);
    a.throws(() => pc.createDataChannel("x", { negotiated: true, id: 65535 }), TypeError);
    a.throws(() => pc.createDataChannel("x".repeat(65536)), TypeError);
    a.throws(() => pc.createDataChannel("x", { protocol: "p".repeat(65536) }), TypeError);
    a.throws(() => pc.createDataChannel("x", { maxRetransmits: 70000 }), TypeError);
    const c = pc.createDataChannel("x", { negotiated: true, id: 7 });
    a.eq(c.id, 7);
    a.throws(() => pc.createDataChannel("y", { negotiated: true, id: 7 }), { type: DOMException, message: /already in use/ });
    const d = pc.createDataChannel("plain");
    a.eq(d.id, null, "an in-band channel has no id before the DTLS role is known");
    a.eq(d.readyState, "connecting");
    a.throws(() => d.send("early"), { type: DOMException, code: 11 });
    a.throws(() => new RTCDataChannel(), TypeError);
    pc.close();
}, { timeoutMs: 10000 });

t.test("maxMessageSize is min(local, remote a=max-message-size; 0 = unlimited) and send past it is a TypeError", async (a) => {
    for (const [adv, pick] of [["1073741823", "local"], ["100000", 100000], ["0", "local"]]) {
        const pa = new RTCPeerConnection(LO), pb = new RTCPeerConnection(LO);
        try {
            trickle(pa, pb);
            const dc = pa.createDataChannel("mm");
            const rp = waitEvent(pb, "datachannel");
            const o = await pa.createOffer();
            await pa.setLocalDescription(o);
            await pb.setRemoteDescription(o);
            const ans = await pb.createAnswer();
            await pb.setLocalDescription(ans);
            await pa.setRemoteDescription({ type: "answer", sdp: ans.sdp.replace(/a=max-message-size:\d+/, "a=max-message-size:" + adv) });
            const local = +/a=max-message-size:(\d+)/.exec(pa.localDescription.sdp)[1];
            const want = pick === "local" ? local : pick;
            a.eq(pa.sctp.maxMessageSize, want, "remote " + adv);
            if (dc.readyState !== "open") await waitEvent(dc, "open");
            const rdc = (await rp).channel;
            if (rdc.readyState !== "open") await waitEvent(rdc, "open");
            a.throws(() => dc.send(new Uint8Array(want + 1)), TypeError, "remote " + adv + ": one byte past");
            a.throws(() => dc.send("x".repeat(want + 1)), TypeError, "remote " + adv + ": string one byte past");
            if (want <= 100000) {
                const got = waitEvent(rdc, "message");
                dc.send(new Uint8Array(want));
                a.eq((await got).data.byteLength, want, "exactly maxMessageSize is delivered");
            }
        } finally { closeAll(pa, pb); }
    }
});


await t.run();
