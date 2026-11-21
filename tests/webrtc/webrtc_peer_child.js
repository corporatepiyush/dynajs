// webrtc_peer_child.js -- the second process for test_webrtc_rtc_data_channel_two_process.js. Signaling is JSON over a loopback
// UDPSocket to the parent's port (argv[1]); argv[2] is "answer" or "offer". Every data-channel message is echoed back
// unchanged (string stays string, binary stays binary); "bye" closes the channel and the child exits once the
// connection is closed.
import { RTCPeerConnection } from "dyna:webrtc";
import { UDPSocket } from "dyna:net";
import { LO } from "./rtc_util.js";

const parentPort = Number(scriptArgs[1]);
const role = scriptArgs[2];
const sig = new UDPSocket({ port: 0, host: "127.0.0.1" });
const send = (o) => sig.send(JSON.stringify(o), "127.0.0.1", parentPort);
const pc = new RTCPeerConnection(LO);

function echo(ch) {
    ch.binaryType = "arraybuffer";
    ch.onmessage = (e) => {
        if (e.data === "bye") { ch.close(); return; }
        ch.send(e.data);
    };
    ch.onclose = () => { pc.close(); sig.close(); };
}

pc.onicecandidate = (e) => send({ t: "cand", c: e.candidate ? e.candidate.toJSON() : null });
pc.ondatachannel = (e) => echo(e.channel);
sig.start({
    message: async (d) => {
        const m = JSON.parse(new TextDecoder().decode(d));
        if (m.t === "desc") {
            await pc.setRemoteDescription(m.d);
            if (m.d.type === "offer") {
                await pc.setLocalDescription(await pc.createAnswer());
                send({ t: "desc", d: pc.localDescription.toJSON() });
            }
        } else if (m.t === "cand") {
            await pc.addIceCandidate(m.c).catch(() => {});
        }
    },
});
send({ t: "hello", pid: 0 });
if (role === "offer") {
    echo(pc.createDataChannel("from-child"));
    await pc.setLocalDescription(await pc.createOffer());
    send({ t: "desc", d: pc.localDescription.toJSON() });
}
