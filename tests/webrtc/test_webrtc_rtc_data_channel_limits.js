// covers: integration dyna:webrtc.RTCPeerConnection.createDataChannel dyna:webrtc.RTCPeerConnection.sctp -- stream limits span
// createDataChannel, the SCTP transport maxChannels and the association (P1 review round 1: R2-C4, R3-1, R1-I12, R4-7).
// - The SCTP association advertises 65535 inbound/outbound streams (RFC 8831 6.2 SHOULD) unless the dynajs test option
//   sctpStreams lowers it; the peer's OS/MIS are stored and maxChannels is the negotiated minimum (webrtc-pc 6.1.1.1).
// - createDataChannel throws OperationError for an id >= maxChannels once connected, and when no id below it is free;
//   a channel created earlier with such an id is closed with an error when the transport connects.
// - Remote channel state is bounded by a per-connection budget (16 MiB counting 1 KiB per channel plus label and
//   protocol bytes); OPENs past it are refused by resetting the stream (RFC 8832 6), so the opener sees close.
// - Stream state is sparse (pages of 32, created only by local sends/resets), so wire stream ids cost nothing
//   (checked in test_rtc_core.c test_sctp_streams).
// - Channel events carry a 32-bit slot: with 65537 channels created before negotiation, the error/close of slot 65536
//   must not land on slot 0 (R2b-4; a 16-bit event id aliased them).
import { RTCPeerConnection } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { LO, waitEvent, delay, connectPair, closeAll } from "./rtc_util.js";

const t = suite("webrtc_data_channel_limits");
const SMALL = Object.freeze({ iceLocalAddresses: ["127.0.0.1"], sctpStreams: 16 });

function opErr(a, f, msg) {
    let e = null;
    try { f(); } catch (x) { e = x; }
    a.ok(e && e.name === "OperationError", msg + " -> " + (e ? e.name + ": " + e.message : "no throw"));
}

t.test("maxChannels is 65535 by default on both sides", async (a) => {
    const { a: x, b: y } = await connectPair();
    a.eq(x.sctp.maxChannels, 65535);
    a.eq(y.sctp.maxChannels, 65535);
    closeAll(x, y);
});

t.test("a peer advertising 16 streams bounds maxChannels and ids on both sides", async (a) => {
    const x = new RTCPeerConnection(SMALL), y = new RTCPeerConnection(LO);
    const early = y.createDataChannel("early", { negotiated: true, id: 30 });
    const errs = [];
    early.onerror = (e) => errs.push(e.error && e.error.message);
    const closed = waitEvent(early, "close", 15000);
    const ok = x.createDataChannel("ok", { negotiated: true, id: 15 });
    const ok2 = y.createDataChannel("ok", { negotiated: true, id: 15 });
    x.onicecandidate = (e) => { if (e.candidate) y.addIceCandidate(e.candidate); };
    y.onicecandidate = (e) => { if (e.candidate) x.addIceCandidate(e.candidate); };
    const o = await x.createOffer(); await x.setLocalDescription(o); await y.setRemoteDescription(o);
    const an = await y.createAnswer(); await y.setLocalDescription(an); await x.setRemoteDescription(an);
    await Promise.all([waitEvent(ok, "open", 15000), waitEvent(ok2, "open", 15000)]);
    a.eq(x.sctp.maxChannels, 16);
    a.eq(y.sctp.maxChannels, 16, "the side advertising 65535 learns the peer's 16");
    await closed;
    a.eq(early.readyState, "closed");
    a.ok(errs.length === 1 && /maxChannels/.test(errs[0]), "early channel on id 30 failed: " + errs.join());
    opErr(a, () => y.createDataChannel("n", { negotiated: true, id: 20 }), "negotiated id 20 >= maxChannels");
    opErr(a, () => y.createDataChannel("n", { negotiated: true, id: 16 }), "negotiated id 16 >= maxChannels");
    const got = waitEvent(ok, "message", 5000);
    ok2.send("hi");
    a.eq((await got).data, "hi", "id 15 carries data");
    let made = 0, threw = null;
    for (let i = 0; i < 20 && !threw; i++) {
        try { y.createDataChannel("auto" + i); made++; } catch (e) { threw = e; }
    }
    a.ok(threw && threw.name === "OperationError", "auto ids run out with OperationError: " + (threw && threw.message));
    a.ok(made >= 6 && made <= 8, "only the ids of one parity below 16 are handed out: " + made);
    closeAll(x, y);
});

t.test("remote OPENs past the per-connection budget are refused by a stream reset", async (a) => {
    const { a: x, b: y } = await connectPair();
    const label = "L".repeat(60000), N = 400;
    let remote = 0, closedLocal = 0, opened = 0;
    y.ondatachannel = () => { remote++; };
    for (let i = 0; i < N; i++) {
        const dc = x.createDataChannel(label);
        dc.onopen = () => { opened++; };
        dc.onclose = () => { closedLocal++; };
    }
    for (let i = 0; i < 600 && remote + closedLocal < N; i++) await delay(25);
    a.eq(remote + closedLocal, N, "every OPEN was either accepted or refused (" + remote + " + " + closedLocal + ")");
    a.ok(remote >= 250 && remote <= 280, "about 16 MiB / 61 KiB channels accepted: " + remote);
    a.ok(closedLocal >= N - 280, "the rest closed on the opener side: " + closedLocal);
    closeAll(x, y);
});
t.test("65537 channels created before negotiation: events for slot 65536 never reach channel 0 (R2b-4)", async (a) => {
    const x = new RTCPeerConnection(SMALL), y = new RTCPeerConnection(LO);
    const first = x.createDataChannel("first");
    const firstEv = [];
    for (const ev of ["error", "closing", "close"]) first.addEventListener(ev, () => firstEv.push(ev));
    const N = 65537;
    let errs = 0, closes = 0;
    let last = null;
    for (let i = 1; i < N; i++) {
        const c = x.createDataChannel("c" + i);
        c.onerror = () => errs++;
        c.onclose = () => closes++;
        last = c;
    }
    let lastClosed = false;
    last.addEventListener("close", () => { lastClosed = true; });
    x.onicecandidate = (e) => { if (e.candidate) y.addIceCandidate(e.candidate); };
    y.onicecandidate = (e) => { if (e.candidate) x.addIceCandidate(e.candidate); };
    const o = await x.createOffer(); await x.setLocalDescription(o); await y.setRemoteDescription(o);
    const an = await y.createAnswer(); await y.setLocalDescription(an); await x.setRemoteDescription(an);
    for (let i = 0; i < 300 && !(lastClosed && first.readyState !== "connecting") && first.readyState !== "closed"; i++) await delay(100);
    await delay(100);
    a.ok(lastClosed, "slot 65536 got its own close");
    a.eq(first.readyState, "open", "channel 0 opened and stays open");
    a.eq(firstEv.join(","), "", "channel 0 got no error/close meant for slot 65536");
    a.ok(errs >= N - 9 && closes >= N - 9, "the channels without an id failed: errors " + errs + " closes " + closes);
    closeAll(x, y);
}, { timeoutMs: 60000 });


await t.run();
