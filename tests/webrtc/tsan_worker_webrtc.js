// flags: --std
// covers: dyna:webrtc in Workers (P1 review concurrency note 4). Four Workers and the main thread each connect a pair
// of RTCPeerConnections over loopback, generate an ECDSA and an RSA certificate (the io-pool offload), exchange 300
// messages and close. Under ThreadSanitizer (tests/ctest/run.sh tsan-workers) any process-global state the stack
// shares (the DTLS BIO method, SSL_CTX, RNG, SCTP slot caches, class ids) shows up as a data race; on an ordinary build
// it checks that every Worker completes. A peer connection cannot leave its thread: postMessage of an
// RTCPeerConnection or RTCDataChannel throws (not structured-cloneable), so no PC is ever used from another Worker.
import * as os from "os";
import * as std from "std";
import { RTCPeerConnection } from "dyna:webrtc";
const N = 300;
const body = `
const LO = { iceLocalAddresses: ["127.0.0.1"] };
async function run() {
    const c1 = await RTCPeerConnection.generateCertificate({ name: "ECDSA", namedCurve: "P-256" });
    const c2 = await RTCPeerConnection.generateCertificate({ name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" });
    const x = new RTCPeerConnection({ ...LO, certificates: [c1] }), y = new RTCPeerConnection({ ...LO, certificates: [c2] });
    x.onicecandidate = (e) => { if (e.candidate) y.addIceCandidate(e.candidate); };
    y.onicecandidate = (e) => { if (e.candidate) x.addIceCandidate(e.candidate); };
    const dc = x.createDataChannel("w");
    let got = 0;
    const done = new Promise((res) => {
        y.ondatachannel = (e) => { e.channel.onmessage = () => { if (++got === ${N}) res(); }; };
    });
    dc.onopen = () => { for (let i = 0; i < ${N}; i++) dc.send("m" + i); };
    const o = await x.createOffer(); await x.setLocalDescription(o); await y.setRemoteDescription(o);
    const a = await y.createAnswer(); await y.setLocalDescription(a); await x.setRemoteDescription(a);
    await Promise.race([done, new Promise((r) => setTimeout(r, 20000))]);
    x.close(); y.close();
    return got;
}
`;
const W = (std.getenv("TMPDIR") || "/tmp") + "/dj_tsan_rtc." + os.getpid() + ".js";
const f = std.open(W, "w");
f.puts(`import * as os from "os";\nimport { RTCPeerConnection } from "dyna:webrtc";\n${body}\nrun().then((n) => os.Worker.parent.postMessage(n), (e) => os.Worker.parent.postMessage("ERR " + e));\n`);
f.close();
const run = new Function("RTCPeerConnection", body + "\nreturn run;")(RTCPeerConnection);
const got = await new Promise((resolve) => {
    const out = [];
    for (let k = 0; k < 4; k++) { const w = new os.Worker(W); w.onmessage = (e) => { out.push(e.data); w.onmessage = null; if (out.length === 4) resolve(out); }; }
    setTimeout(() => resolve(["TIMEOUT", out]), 60000);
});
const mine = await run();
os.remove(W);
let refused = 0;
const probe = new os.Worker("data:text/javascript,");
const pc = new RTCPeerConnection();
const ch = pc.createDataChannel("p");
for (const v of [pc, ch]) { try { probe.postMessage(v); } catch (e) { refused++; } }
pc.close();
probe.onmessage = null;
const ok = JSON.stringify(got) === JSON.stringify([N, N, N, N]) && mine === N && refused === 2;
print("tsan_worker_webrtc: " + (ok ? "ok" : "FAILED " + JSON.stringify(got) + " main " + mine + " refused " + refused) + " (4 workers + main, " + N + " messages each)");
if (!ok) std.exit(1);
std.exit(0);
