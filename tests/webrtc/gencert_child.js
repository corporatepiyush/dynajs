// covers: dyna:webrtc.RTCPeerConnection.generateCertificate -- child process for the states suite: generates two RSA 2048
// and two ECDSA P-256 certificates and prints "gencert: <count> <timer ticks during generation>" so the parent can run it
// with --io-threads 0 (inline fallback on the loop thread) and with the default pool.
import { RTCPeerConnection } from "dyna:webrtc";
const rsa = { name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" };
const ec = { name: "ECDSA", namedCurve: "P-256" };
let ticks = 0, ticking = true;
const tick = () => { ticks++; if (ticking) setTimeout(tick, 0); };
setTimeout(tick, 0);
const certs = await Promise.all([rsa, rsa, ec, ec].map((a) => RTCPeerConnection.generateCertificate(a)));
ticking = false;
const ok = certs.filter((c) => c.getFingerprints()[0].value.length === 95).length;
print("gencert: " + ok + " " + ticks);
