// webrtc_shutdown_child.js -- child for test_webrtc_rtc_peer_connection_shutdown.js: leaves RTCPeerConnections alive in a given state
// and then ends the process abruptly (uncaught error or rejected top-level await), so the engine teardown sweep runs
// against live transports, timers, TURN transactions and data channels.
import { RTCPeerConnection } from "dyna:webrtc";
import { LO, connectPair, delay } from "./rtc_util.js";

const shape = scriptArgs[1];
if (shape === "gathering-throw") {
    const pc = new RTCPeerConnection({ ...LO, iceServers: [{ urls: "turn:127.0.0.1:9", username: "u", credential: "p" }] });
    pc.createDataChannel("x");
    await pc.setLocalDescription(await pc.createOffer());
    await delay(600);
    console.log("READY " + pc.iceGatheringState);
    throw new Error("child boom");
} else if (shape === "connected-throw") {
    const { dc } = await connectPair();
    dc.send(new Uint8Array(300000));
    console.log("READY " + dc.readyState);
    throw new Error("child boom");
} else if (shape === "connected-reject") {
    const { dc } = await connectPair();
    console.log("READY " + dc.readyState);
    await Promise.reject(new Error("child reject"));
} else if (shape === "fresh-throw") {
    new RTCPeerConnection(LO);
    console.log("READY new");
    throw new Error("child boom");
} else if (shape === "closed-throw") {
    const { a, b } = await connectPair();
    a.close();
    b.close();
    console.log("READY closed");
    throw new Error("child boom");
}
else if (shape === "keygen-close") {
    const pc = new RTCPeerConnection(LO);
    pc.createDataChannel("x");
    let st = "pending";
    const fresh = new RTCPeerConnection(LO);
    fresh.createOffer().then(() => { st = "resolved"; }, (e) => { st = "rejected " + e.name; });
    let st2 = "pending";
    fresh.createOffer().then(() => { st2 = "resolved"; }, (e) => { st2 = "rejected " + e.name; });
    await null;
    await null;
    const fresh2 = new RTCPeerConnection(LO);
    fresh2.createDataChannel("y");
    let st3 = "pending";
    fresh2.setLocalDescription().then(() => { st3 = "resolved"; }, (e) => { st3 = "rejected " + e.name; });
    await null;
    await null;
    fresh2.close();
    fresh.close();
    const o = await pc.createOffer();
    const o2 = await pc.createOffer();
    const fp = (s) => (/a=fingerprint:\S+ (\S+)/.exec(s) || [])[1];
    await delay(300);
    console.log("KEYGEN " + JSON.stringify({ st, st2, st3, sig: fresh.signalingState, fp: !!fp(o.sdp) && fp(o.sdp) === fp(o2.sdp) }));
    pc.close();
} else if (shape === "gencert-throw") {
    for (let i = 0; i < 8; i++) RTCPeerConnection.generateCertificate({ name: "RSASSA-PKCS1-v1_5", modulusLength: 2048 }).catch(() => {});
    await null;
    console.log("READY gencert");
    throw new Error("child boom");
} else if (shape === "dc-half-throw") {
    const pc = new RTCPeerConnection(LO);
    pc.createDataChannel("x");
    const y = pc.createDataChannel("y");
    y.close();
    await delay(50);
    console.log("READY half " + y.readyState);
    throw new Error("child boom");
} else if (shape === "connected-dcclose-throw") {
    const { dc } = await connectPair();
    const closed = new Promise((r) => { dc.onclose = r; });
    dc.close();
    await closed;
    console.log("READY dcclosed " + dc.readyState);
    throw new Error("child boom");
} else if (shape === "dc-throw") {
    const pc = new RTCPeerConnection(LO);
    pc.createDataChannel("x");
    console.log("READY dc");
    throw new Error("child boom");
} else if (shape === "keygen-throw") {
    const pcs = [];
    for (let i = 0; i < 8; i++) {
        const pc = new RTCPeerConnection(LO);
        pc.createDataChannel("x");
        pc.createOffer().catch(() => {});
        pcs.push(pc);
    }
    await null;
    console.log("READY keygen");
    throw new Error("child boom");
}
