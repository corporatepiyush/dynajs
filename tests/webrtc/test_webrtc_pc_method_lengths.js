// covers: dyna:webrtc.RTCPeerConnection -- WebIDL Function.length of each RTCPeerConnection operation (webrtc-pc 4.4.x, WebIDL 3.7.6):
// an operation whose arguments are all optional has length 0 (createOffer, createAnswer, setLocalDescription, addIceCandidate, getStats).
import { RTCPeerConnection } from "dyna:webrtc";

const want = { createOffer: 0, createAnswer: 0, setLocalDescription: 0, setRemoteDescription: 1, addIceCandidate: 0, getStats: 0, createDataChannel: 1, close: 0 };
let fails = 0;
for (const [k, v] of Object.entries(want)) {
    const f = RTCPeerConnection.prototype[k];
    const got = typeof f === "function" ? f.length : "missing";
    if (got !== v) { print("FAIL RTCPeerConnection.prototype." + k + ".length " + got + ", want " + v); fails++; }
}
if (fails) throw new Error(fails + " length mismatch(es)");
print("test_webrtc_pc_method_lengths: ok");
