// covers: dyna:webrtc.RTCPeerConnection -- ICE over non-loopback interfaces with the default configuration: host
// candidates on every up non-loopback IPv4 interface (RFC 8445 5.1.1.1: loopback is never a candidate unless it is
// the only interface), and two in-process peers connecting over the interface IP. Runs when WEBRTC_LAN=1, which the
// Linux container (docker/linux.sh with DYNA_COMPAT_ONLY=1) sets; skipped elsewhere, because a host firewall such as
// the macOS application firewall in block-all/stealth mode drops inbound LAN datagrams for every process.
import { RTCPeerConnection } from "dyna:webrtc";
import { Exec, getEnv } from "dyna:sys";
import { suite } from "../lib/kit.js";
import { connectPair, closeAll } from "./rtc_util.js";

const t = suite("webrtc_lan");
const ON = getEnv("WEBRTC_LAN") === "1";

function lanAddrs() {
    let r = Exec("/bin/sh", ["-c", "ip -4 -o addr show up 2>/dev/null || ifconfig 2>/dev/null || hostname -I 2>/dev/null"]);
    const out = String(r.stdout);
    const ips = new Set();
    const re = /inet /.test(out) ? /inet (?:addr:)?(\d+\.\d+\.\d+\.\d+)/g : /(?:^|\s)(\d+\.\d+\.\d+\.\d+)(?=\s|$)/g;
    for (const m of out.matchAll(re)) if (!m[1].startsWith("127.") && !m[1].startsWith("169.254.")) ips.add(m[1]);
    return [...ips].sort();
}

async function gather(cfg) {
    const pc = new RTCPeerConnection(cfg);
    pc.createDataChannel("g");
    const cands = [];
    const done = new Promise((res) => { pc.onicecandidate = (e) => { if (!e.candidate) res(); else if (e.candidate.candidate !== "") cands.push(e.candidate); }; });
    await pc.setLocalDescription(await pc.createOffer());
    await done;
    pc.close();
    return cands;
}

t.test("default configuration gathers a host candidate on every non-loopback IPv4 interface and none on loopback", async (a) => {
    if (!ON) a.skip("WEBRTC_LAN=1 not set (run in docker/linux.sh)");
    const want = lanAddrs();
    a.ok(want.length >= 1, "the host has a non-loopback IPv4 address");
    const cands = await gather({});
    const got = [...new Set(cands.filter((c) => c.type === "host" && c.address.includes(".")).map((c) => c.address))].sort();
    a.deep(got, want);
    a.ok(!cands.some((c) => c.address.startsWith("127.")), "no loopback candidate");
});

t.test("two peers with the default configuration connect over the LAN address and move a 1 MiB message", async (a) => {
    if (!ON) a.skip("WEBRTC_LAN=1 not set (run in docker/linux.sh)");
    const { a: pa, b: pb, dc, rdc } = await connectPair({ configA: {}, configB: {}, ms: 15000 });
    try {
        const st = await pa.getStats();
        let pair = null;
        for (const r of st.values()) if (r.type === "candidate-pair" && r.nominated && r.state === "succeeded") pair = r;
        a.must(pair, "nominated pair");
        const lc = st.get(pair.localCandidateId), rc = st.get(pair.remoteCandidateId);
        a.ok(!lc.address.startsWith("127.") && !rc.address.startsWith("127."), "pair is on the LAN: " + lc.address + " -> " + rc.address);
        a.ok(lanAddrs().includes(lc.address));
        rdc.binaryType = "arraybuffer";
        const big = new Uint8Array(1 << 20);
        for (let i = 0; i < big.length; i++) big[i] = i % 251;
        const got = new Promise((res) => { rdc.onmessage = (e) => res(new Uint8Array(e.data)); });
        dc.send(big);
        const rb = await got;
        a.eq(rb.length, big.length);
        let same = true;
        for (let i = 0; i < rb.length; i++) if (rb[i] !== big[i]) { same = false; break; }
        a.ok(same);
    } finally { closeAll(pa, pb); }
}, { timeoutMs: 30000 });

t.test("iceLocalAddresses restricts gathering to the listed LAN address", async (a) => {
    if (!ON) a.skip("WEBRTC_LAN=1 not set (run in docker/linux.sh)");
    const ip = lanAddrs()[0];
    const cands = await gather({ iceLocalAddresses: [ip] });
    a.eq(cands.length, 1);
    a.eq(cands[0].address, ip);
});

await t.run();
