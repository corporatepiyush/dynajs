// covers: dyna:webrtc.RTCDTMFSender -- insertDTMF, toneBuffer, canInsertDTMF, tonechange timing (webrtc-pc 7, RFC 4733 negotiation)
// dyna:webrtc RTCDTMFSender between two in-process RTCPeerConnections over loopback, written by hand from the intent of
// the WPT webrtc tests (RTCDTMFSender-insertDTMF, RTCDTMFSender-ontonechange, RTCDTMFSender-ontonechange-long,
// RTCPeerConnection-insertDTMF / canInsertDTMF), no download. Spec choices pinned (webrtc-pc 7.2):
//   - canInsertDTMF is the "determine if DTMF can be sent" algorithm: false unless the connection is "connected", the
//     transceiver is not stopping, the sender has a track, currentDirection is sendrecv or sendonly, the first encoding
//     is active and audio/telephone-event (RFC 4733) is among the negotiated codecs;
//   - insertDTMF: InvalidStateError when DTMF cannot be sent (checked before the characters), InvalidCharacterError for
//     anything outside 0-9 A-D a-d # * ',' (toneBuffer unchanged); a-d become A-D; duration is clamped to 40..6000 ms
//     (default 100) and interToneGap to 30..6000 ms (default 70); a call while tones play replaces the buffer and does
//     not start a second playout task;
//   - the playout task removes one tone, schedules itself after duration + interToneGap (2000 ms for ','), then fires
//     tonechange with that tone; an empty buffer fires tonechange with tone "" once and stops; when DTMF can no longer
//     be sent the task stops without an event.
// RFC 4733 tone packets on the wire (event codes, durations, E bit, segments) are pinned in harness_media_e2e.js; this
// suite pins the queue, timing and events only. Timings are checked
// with a lower bound (never early; 5 ms slack: an event is fired just after its task rescheduled, so a late
// dispatch of one tonechange shortens the measured gap to the next)  and a loose upper bound for loaded machines.
import { RTCPeerConnection, RTCDTMFSender, RTCRtpReceiver } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { LO, trickle, negotiate, waitState, closeAll, delay } from "./rtc_util.js";

const t = suite("webrtc_dtmf_sender");

function track(kind) {
    const pc = new RTCPeerConnection(LO);
    const tr = pc.addTransceiver(kind).receiver.track;
    pc.close();
    return tr;
}
function thr(a, fn, name, msg) {
    try { fn(); a.ok(false, (msg || name) + ": did not throw"); }
    catch (e) { a.eq(e && e.name, name, msg); }
}
async function pair(opts = {}) {
    const x = new RTCPeerConnection(LO), y = new RTCPeerConnection(LO);
    trickle(x, y);
    const tr = x.addTransceiver(track("audio"), { direction: opts.direction || "sendrecv" });
    if (opts.prefs) y.ontrack = (e) => e.transceiver.setCodecPreferences(opts.prefs);
    await x.setLocalDescription(await x.createOffer());
    await y.setRemoteDescription(x.localDescription);
    await y.setLocalDescription(await y.createAnswer());
    await x.setRemoteDescription(y.localDescription);
    await Promise.all([waitState(x, "connected"), waitState(y, "connected")]);
    return { x, y, tr, d: tr.sender.dtmf };
}
function recorder(d) {
    const t0 = Date.now(), evs = [];
    d.ontonechange = (e) => evs.push({ tone: e.tone, at: Date.now() - t0 });
    return evs;
}
function waitTone(d, tone, ms = 15000) {
    return new Promise((res, rej) => {
        const to = setTimeout(() => rej(new Error("no tonechange " + JSON.stringify(tone))), ms);
        d.addEventListener("tonechange", function h(e) {
            if (e.tone !== tone) return;
            clearTimeout(to);
            d.removeEventListener("tonechange", h);
            res(e);
        });
    });
}

t.test("before connected: an RTCDTMFSender that cannot send (InvalidStateError before the character check)", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const d = pc.addTransceiver(track("audio")).sender.dtmf;
    a.ok(d instanceof RTCDTMFSender);
    a.eq(d.canInsertDTMF, false);
    a.eq(d.toneBuffer, "");
    thr(a, () => d.insertDTMF("1"), "InvalidStateError");
    thr(a, () => d.insertDTMF("X"), "InvalidStateError", "the state check comes first");
    a.eq(d.toneBuffer, "");
    a.throws(() => new RTCDTMFSender(), TypeError);
    a.eq(globalThis.RTCDTMFSender, RTCDTMFSender);
    a.throws(() => { "use strict"; d.toneBuffer = "1"; }, TypeError, "toneBuffer is readonly");
    pc.close();
});

t.test("connected with telephone-event: canInsertDTMF, characters, normalisation, events in order with spec timing", async (a) => {
    const { x, y, d } = await pair();
    a.match(x.remoteDescription.sdp, /a=rtpmap:\d+ telephone-event\/48000/, "RFC 4733 negotiated");
    a.eq(d.canInsertDTMF, true);
    thr(a, () => d.insertDTMF("12E"), "InvalidCharacterError");
    thr(a, () => d.insertDTMF("1 2"), "InvalidCharacterError", "spaces are not tones");
    a.eq(d.toneBuffer, "", "a rejected call leaves the buffer");
    const evs = recorder(d);
    d.insertDTMF("1a#*,b", 50, 30);
    a.eq(d.toneBuffer, "1A#*,B", "a-d become A-D");
    await waitTone(d, "");
    a.deep(evs.map((e) => e.tone), ["1", "A", "#", "*", ",", "B", ""]);
    a.eq(d.toneBuffer, "");
    const gap = (i) => evs[i + 1].at - evs[i].at;
    for (const i of [0, 1, 2, 3, 5]) {
        a.ok(gap(i) >= 75, "tone " + evs[i].tone + ": duration 50 + gap 30 before the next (" + gap(i) + " ms)");
        a.ok(gap(i) < 1500, "not stalled (" + gap(i) + " ms)");
    }
    a.ok(gap(4) >= 1990 && gap(4) < 3500, "',' waits 2000 ms (" + gap(4) + ")");
    a.ok(evs[0].at < 500, "the first tone starts in a following task");
    closeAll(x, y);
}, { timeoutMs: 20000 });

t.test("duration and interToneGap clamp to 40..6000 and 30..6000; defaults 100 / 70", async (a) => {
    const { x, y, d } = await pair();
    let evs = recorder(d);
    d.insertDTMF("12", 1, 1);
    await waitTone(d, "");
    a.ok(evs[1].at - evs[0].at >= 65 && evs[1].at - evs[0].at < 1500, "40 + 30 (" + (evs[1].at - evs[0].at) + ")");
    evs = recorder(d);
    d.insertDTMF("12");
    await waitTone(d, "");
    a.ok(evs[1].at - evs[0].at >= 165, "100 + 70 by default (" + (evs[1].at - evs[0].at) + ")");
    evs = recorder(d);
    d.insertDTMF("1", 6001, 0);
    await delay(3000);
    a.deep(evs.map((e) => e.tone), ["1"], "6000 + 30: nothing else within 3 s");
    d.insertDTMF("");
    closeAll(x, y);
}, { timeoutMs: 30000 });

t.test("a call while tones play replaces the buffer without a second task; \"\" ends after one empty tonechange", async (a) => {
    const { x, y, d } = await pair();
    const evs = recorder(d);
    d.insertDTMF("123456", 40, 30);
    await waitTone(d, "2");
    d.insertDTMF("9", 40, 30);
    a.eq(d.toneBuffer, "9");
    await waitTone(d, "");
    a.deep(evs.map((e) => e.tone), ["1", "2", "9", ""], "the rest of the old buffer is dropped");
    const at = evs.map((e) => e.at);
    for (let i = 1; i < at.length; i++) a.ok(at[i] - at[i - 1] >= 65, "one task: never two tones within 70 ms (" + (at[i] - at[i - 1]) + ")");
    evs.length = 0;
    d.insertDTMF("5", 40, 30);
    await waitTone(d, "5");
    d.insertDTMF("");
    await waitTone(d, "");
    await delay(300);
    a.deep(evs.map((e) => e.tone), ["5", ""], "an emptied buffer fires one empty tonechange");
    closeAll(x, y);
}, { timeoutMs: 20000 });

t.test("canInsertDTMF false without telephone-event, recvonly, no track, inactive encoding, stopped, closed", async (a) => {
    const audio = RTCRtpReceiver.getCapabilities("audio").codecs;
    let s = await pair({ prefs: [audio[0]] });
    a.ok(!/telephone-event/.test(s.x.remoteDescription.sdp), "the answer kept opus only");
    a.eq(s.d.canInsertDTMF, false, "telephone-event not negotiated");
    thr(a, () => s.d.insertDTMF("1"), "InvalidStateError");
    closeAll(s.x, s.y);
    s = await pair({ direction: "recvonly" });
    a.eq(s.d.canInsertDTMF, false, "recvonly");
    closeAll(s.x, s.y);
    s = await pair();
    a.eq(s.d.canInsertDTMF, true);
    const keep = s.tr.sender.track;
    await s.tr.sender.replaceTrack(null);
    a.eq(s.d.canInsertDTMF, false, "no track");
    await s.tr.sender.replaceTrack(keep);
    a.eq(s.d.canInsertDTMF, true);
    const p = s.tr.sender.getParameters();
    p.encodings[0].active = false;
    await s.tr.sender.setParameters(p);
    a.eq(s.d.canInsertDTMF, false, "first encoding inactive");
    const q = s.tr.sender.getParameters();
    q.encodings[0].active = true;
    await s.tr.sender.setParameters(q);
    a.eq(s.d.canInsertDTMF, true);
    const evs = recorder(s.d);
    s.d.insertDTMF("123", 40, 30);
    await waitTone(s.d, "1");
    s.tr.stop();
    a.eq(s.d.canInsertDTMF, false, "stopping");
    await delay(400);
    a.deep(evs.map((e) => e.tone), ["1"], "the playout task stops without an event once DTMF cannot be sent");
    a.eq(s.d.toneBuffer, "23", "the remaining tones stay in the buffer");
    closeAll(s.x, s.y);
    s = await pair();
    s.x.close();
    a.eq(s.d.canInsertDTMF, false, "closed");
    s.y.close();
}, { timeoutMs: 30000 });

t.test("EventTarget surface: addEventListener, dispatchEvent, handler identity", async (a) => {
    const pc = new RTCPeerConnection(LO);
    const d = pc.addTransceiver("audio").sender.dtmf;
    const got = [];
    const f = (e) => got.push("h:" + e.type);
    d.ontonechange = f;
    a.eq(d.ontonechange, f);
    d.addEventListener("tonechange", (e) => got.push("l:" + e.type));
    a.eq(d.dispatchEvent({ type: "tonechange" }), true);
    a.deep(got, ["h:tonechange", "l:tonechange"]);
    d.ontonechange = null;
    a.eq(d.ontonechange, null);
    pc.close();
});

await t.run();
