// covers: dyna:webrtc.RTCPeerConnection -- STUN server-reflexive gathering (RFC 8489 Binding, RFC 8445
// 5.1.1.2), TURN relayed candidates (RFC 8656 Allocate with the long-term credential 401 challenge, CreatePermission,
// ChannelBind/ChannelData, Refresh LIFETIME 0 on close), a data channel carried only over relays under
// iceTransportPolicy "relay", and icecandidateerror for wrong credentials and an unreachable server. The server is the
// independent JS fixture turn_server.js on 127.0.0.1; mappedPortDelta stands in for a NAT so srflx differs from host.
// R4-5: setConfiguration replaces iceServers, iceTransportPolicy and the dynajs iceLocalAddresses for the next gathering
// phase; an ICE restart gathers again: new interfaces get sockets, new servers get allocations, and under "relay" only
// relay candidates are signalled and paired (webrtc-pc 4.4.1.6, RFC 8829 3.5.3). Existing candidates stay until then.
// A server listed again with the same url/username/credential keeps its allocation; "relay" -> "all" brings back the
// host and srflx candidates.
// A server dropped from iceServers is released once the restart selects a pair (Refresh LIFETIME 0), not when the restart
// offer is applied, so a relayed pair keeps carrying data meanwhile (DEC-a); its relay is not signalled again. Released
// sessions free their slot; at most 64 live sessions, and a server over that limit gets icecandidateerror 701 with an
// errorText naming the limit instead of being skipped silently.
// The selection that releases orphans keeps the one session carrying the selected pair: after a restart no pair is formed
// on a released candidate, so only a peer renominating an older pair while the restart is staged reaches that branch; dynajs
// never renominates, so this file cannot exercise it (removing it stays green here; it guards interop with renominating peers).
// R1b-9: a policy set by setConfiguration takes effect at the next gathering (the restart), not at once: a re-offer
// without restart still lists the host candidates and the host pair keeps working (JSEP 4.1.18).
// Round 3: a remote restart applied at SLD(answer) regathers; the agent forgets that local gathering had ended (local_done follows
// gather_done to 0), so with a silent STUN server still pending it stays "connected", never "completed" (RFC 8445 6.1.3 needs both
// ends done).
// R2b-2: coturn-style stun:H:P plus turn:H:P share one server address; relay data no session owns is never consumed by
// the STUN session, so the relayed pair still connects and echoes.
// Round 4 (DEC-a streaming stalled ~2 s in 1 of 13 ASan runs): the tick passes its own now while a send in the same tick
// stamps a later clock, so "now - last use" wrapped and a just-used TURN permission was evicted as 300 s idle; the server
// kept relaying on its channel and the client discarded it. Idle eviction now needs now past the last use. ChannelData that
// overtakes the ChannelBind success response is accepted (RFC 8656 12.1 SHOULD). After a restart selects, the old remote stays
// a DTLS source until a datagram arrives from the selected remote, so the peer's in-flight old-path data is not dropped.
// The unreachable STUN server is a bound UDP socket that never answers: a port closed after probing could be rebound by
// another suite's ICE agent running in parallel, which answers the Binding with 400 instead of staying silent.
// The 64-session case exchanges no candidates: with none, no restart can select a pair (which would release the
// previous sessions), so the count does not depend on how fast loopback ICE connects under parallel load.
import { RTCPeerConnection } from "dyna:webrtc";
import { UDPSocket } from "dyna:net";
import { suite } from "../lib/kit.js";
import { LO, waitEvent, waitState, closeAll, delay, trickle, negotiate, connectPair } from "./rtc_util.js";
import { TurnServer } from "./turn_server.js";

const t = suite("webrtc_ice_servers");

function gatherAll(pc, ms = 10000) {
    return new Promise((res, rej) => {
        const cands = [], errors = [];
        const tm = setTimeout(() => rej(new Error("gathering timeout, got " + cands.length)), ms);
        pc.addEventListener("icecandidateerror", (e) => errors.push(e));
        pc.addEventListener("icecandidate", (e) => {
            if (!e.candidate) { clearTimeout(tm); res({ cands, errors }); return; }
            if (e.candidate.candidate === "") return;
            cands.push(e.candidate);
        });
    });
}

async function offerGather(cfg, ms) {
    const pc = new RTCPeerConnection(cfg);
    pc.createDataChannel("g");
    const g = gatherAll(pc, ms);
    await pc.setLocalDescription(await pc.createOffer());
    const r = await g;
    return { pc, ...r };
}

function blackHole() {
    return new UDPSocket({ port: 0, host: "127.0.0.1" });
}

t.test("stun srflx candidate carries the mapped address and raddr/rport of its base", async (a) => {
    const srv = new TurnServer({ mappedPortDelta: 1000 });
    try {
        const { pc, cands, errors } = await offerGather({ ...LO, iceServers: [{ urls: "stun:127.0.0.1:" + srv.port }] });
        const host = cands.filter((c) => c.type === "host");
        const srflx = cands.filter((c) => c.type === "srflx");
        a.eq(errors.length, 0);
        a.eq(host.length, 1);
        a.eq(srflx.length, 1);
        a.eq(srflx[0].address, "127.0.0.1");
        a.eq(srflx[0].port, (host[0].port + 1000) & 0xffff, "the fixture NAT adds 1000 to the 16-bit port");
        a.eq(srflx[0].relatedAddress, host[0].address);
        a.eq(srflx[0].relatedPort, host[0].port);
        a.ok(srflx[0].priority < host[0].priority);
        a.ok(srv.stats.binding >= 1);
        a.match(pc.localDescription.sdp, / typ srflx raddr 127\.0\.0\.1 rport \d+/);
        pc.close();
    } finally { srv.close(); }
});

t.test("stun mapped address equal to the host address yields no redundant srflx", async (a) => {
    const srv = new TurnServer();
    try {
        const { pc, cands } = await offerGather({ ...LO, iceServers: [{ urls: "stun:127.0.0.1:" + srv.port }] });
        a.eq(cands.filter((c) => c.type === "srflx").length, 0);
        a.eq(cands.filter((c) => c.type === "host").length, 1);
        a.ok(srv.stats.binding >= 1);
        pc.close();
    } finally { srv.close(); }
});

t.test("turn allocation answers the 401 challenge and yields a relay candidate", async (a) => {
    const srv = new TurnServer({ mappedPortDelta: 7 });
    try {
        const { pc, cands, errors } = await offerGather({ ...LO,
            iceServers: [{ urls: "turn:127.0.0.1:" + srv.port + "?transport=udp", username: "user", credential: "pass" }] });
        const relay = cands.filter((c) => c.type === "relay");
        a.eq(errors.length, 0);
        a.eq(relay.length, 1);
        a.eq(relay[0].address, "127.0.0.1");
        a.eq(relay[0].protocol, "udp");
        a.eq(srv.stats.alloc401, 1);
        a.eq(srv.stats.allocOk, 1);
        a.eq(srv.stats.badMI, 0);
        a.eq(cands.filter((c) => c.type === "srflx").length, 1);
        pc.close();
        await delay(100);
        a.eq(srv.stats.dealloc, 1);
        a.eq(srv.allocs.size, 0);
    } finally { srv.close(); }
});

t.test("iceTransportPolicy relay gathers only relay candidates", async (a) => {
    const srv = new TurnServer({ mappedPortDelta: 3 });
    try {
        const { pc, cands } = await offerGather({ ...LO, iceTransportPolicy: "relay",
            iceServers: [{ urls: "turn:127.0.0.1:" + srv.port, username: "user", credential: "pass" }] });
        a.ok(cands.length >= 1);
        for (const c of cands) a.eq(c.type, "relay");
        a.ok(!/typ host|typ srflx/.test(pc.localDescription.sdp));
        pc.close();
    } finally { srv.close(); }
});

t.test("data channel over TURN relays on both sides (policy relay) moves messages through the server", async (a) => {
    const srv = new TurnServer();
    const cfg = { ...LO, iceTransportPolicy: "relay",
        iceServers: [{ urls: "turn:127.0.0.1:" + srv.port, username: "user", credential: "pass" }] };
    const pa = new RTCPeerConnection(cfg), pb = new RTCPeerConnection(cfg);
    try {
        trickle(pa, pb);
        const dc = pa.createDataChannel("relay");
        const remote = waitEvent(pb, "datachannel", 15000);
        remote.catch(() => {});
        await negotiate(pa, pb);
        await Promise.all([waitState(pa, "connected", 15000), waitState(pb, "connected", 15000)]);
        const rdc = (await remote).channel;
        if (dc.readyState !== "open") await waitEvent(dc, "open", 10000);
        if (rdc.readyState !== "open") await waitEvent(rdc, "open", 10000);
        const got = [];
        const done = new Promise((res) => rdc.addEventListener("message", (e) => { got.push(e.data); if (got.length === 50) res(); }));
        const big = new Uint8Array(100000);
        for (let i = 0; i < big.length; i++) big[i] = i * 31;
        for (let i = 0; i < 49; i++) dc.send("m" + i);
        dc.send(big);
        await done;
        for (let i = 0; i < 49; i++) a.eq(got[i], "m" + i);
        const rb = new Uint8Array(got[49]);
        a.eq(rb.length, big.length);
        let same = true;
        for (let i = 0; i < rb.length; i++) if (rb[i] !== big[i]) { same = false; break; }
        a.ok(same);
        a.eq(srv.stats.allocOk, 2);
        a.ok(srv.stats.perm + srv.stats.chan >= 1);
        a.ok(srv.stats.sendInd + srv.stats.chanDataIn > 50);
        a.ok(srv.stats.dataInd + srv.stats.chanDataOut > 50);
        const st = await pa.getStats();
        let pair = null;
        for (const r of st.values()) if (r.type === "candidate-pair" && r.nominated && r.state === "succeeded") pair = r;
        a.must(pair, "a nominated succeeded pair");
        a.eq(st.get(pair.localCandidateId).candidateType, "relay");
        a.eq(st.get(pair.remoteCandidateId).candidateType, "relay");
    } finally { closeAll(pa, pb); await delay(50); srv.close(); }
}, { timeoutMs: 40000 });

t.test("wrong TURN credential fires icecandidateerror 401 and gathering still completes", async (a) => {
    const srv = new TurnServer();
    try {
        const url = "turn:127.0.0.1:" + srv.port;
        const { pc, cands, errors } = await offerGather({ ...LO, iceServers: [{ urls: url, username: "user", credential: "nope" }] });
        a.eq(errors.length, 1);
        a.eq(errors[0].errorCode, 401);
        a.eq(errors[0].url, url);
        a.eq(typeof errors[0].errorText, "string");
        a.eq(cands.filter((c) => c.type === "relay").length, 0);
        a.eq(cands.filter((c) => c.type === "host").length, 1);
        a.eq(srv.stats.allocOk, 0);
        a.ok(srv.stats.badMI >= 1);
        a.eq(pc.iceGatheringState, "complete");
        pc.close();
    } finally { srv.close(); }
});

t.test("unreachable STUN server fires icecandidateerror 701 after the RFC 8489 schedule (39.5 s) and gathering completes", async (a) => {
    const hole = blackHole();
    try {
        const url = "stun:127.0.0.1:" + hole.port;
        const { pc, cands, errors } = await offerGather({ ...LO, iceServers: [{ urls: url }] }, 45000);
        a.eq(errors.length, 1);
        a.eq(errors[0].errorCode, 701);
        a.eq(errors[0].url, url);
        a.eq(cands.filter((c) => c.type === "host").length, 1);
        pc.close();
    } finally { hole.close(); }
}, { timeoutMs: 50000 });

t.test("iceServers validation follows the W3C 'validate an ICE server URL' algorithm", async (a) => {
    const mk = (s) => () => new RTCPeerConnection({ iceServers: [s] });
    const C = { username: "u", credential: "p" };
    const syntax = ["http://127.0.0.1", "stun:", "stun://127.0.0.1", "stun:u@127.0.0.1", "stun:127.0.0.1/x", "stun:127.0.0.1#f",
        "stun:127.0.0.1?transport=udp", "turn:127.0.0.1?transport=sctp", "turn:127.0.0.1:0", "turn:127.0.0.1:70000", "turn:[::1"];
    for (const u of syntax) a.throws(mk({ urls: u, ...C }), { type: DOMException, code: 12 }, u);
    a.throws(mk({ urls: [] }), { type: DOMException, code: 12 });
    a.throws(mk({ urls: "turns:127.0.0.1:5349", ...C }), { type: DOMException, message: /not supported/ });
    a.throws(mk({ urls: "stuns:127.0.0.1" }), { type: DOMException, message: /not supported/ });
    a.throws(mk({ urls: "turns:127.0.0.1?transport=x", ...C }), { type: DOMException, message: /not supported/ });
    a.throws(mk({ urls: "turn:127.0.0.1:3478" }), { type: DOMException, code: 15 });
    a.throws(mk({ urls: "turn:127.0.0.1:3478", username: "u" }), { type: DOMException, code: 15 });
    a.throws(mk({}), TypeError);
    for (const u of ["stun:127.0.0.1", "stun:127.0.0.1:19302", "stun:[::1]:3478", "turn:127.0.0.1?transport=udp", "turn:127.0.0.1?transport=tcp"])
        new RTCPeerConnection({ iceServers: [{ urls: u, ...C }] }).close();
    a.ok(true);
});

t.test("a server no local socket can reach fires 701 at once; a TCP-only TURN server is accepted and skipped", async (a) => {
    const { pc, cands, errors } = await offerGather({ ...LO, iceServers: [{ urls: "stun:[::1]:3478" },
        { urls: "turn:127.0.0.1:9?transport=tcp", username: "u", credential: "p" }] }, 3000);
    a.eq(errors.length, 1);
    a.eq(errors[0].errorCode, 701);
    a.eq(errors[0].url, "stun:[::1]:3478");
    a.eq(cands.length, 1);
    a.eq(cands[0].type, "host");
    pc.close();
});

t.test("setConfiguration iceServers + relay policy take effect at the ICE restart: relay gathered and selected (R4-5)", async (a) => {
    const srv = new TurnServer();
    const pa = new RTCPeerConnection(LO), pb = new RTCPeerConnection(LO);
    try {
        trickle(pa, pb);
        const dc = pa.createDataChannel("r");
        const remote = waitEvent(pb, "datachannel", 15000);
        remote.catch(() => {});
        await negotiate(pa, pb);
        await Promise.all([waitState(pa, "connected", 15000), waitState(pb, "connected", 15000)]);
        const rdc = (await remote).channel;
        if (dc.readyState !== "open") await waitEvent(dc, "open", 10000);
        a.eq(srv.stats.allocOk, 0, "no allocation before setConfiguration");
        const cfg = { ...LO, iceTransportPolicy: "relay", iceServers: [{ urls: "turn:127.0.0.1:" + srv.port, username: "user", credential: "pass" }] };
        pa.setConfiguration(cfg);
        pb.setConfiguration(cfg);
        a.eq(pa.getConfiguration().iceTransportPolicy, "relay");
        a.throws(() => pa.setConfiguration({ ...LO, iceServers: [{ urls: "bogus:x" }] }), {});
        a.eq(pa.getConfiguration().iceTransportPolicy, "relay", "a rejected configuration changes nothing");
        const types = [];
        pa.addEventListener("icecandidate", (e) => { if (e.candidate && e.candidate.candidate) types.push(e.candidate.type); });
        pa.restartIce();
        await negotiate(pa, pb);
        let pair = null, st = null;
        for (let i = 0; i < 100 && !pair; i++) {
            st = await pa.getStats();
            for (const r of st.values())
                if (r.type === "candidate-pair" && r.nominated && r.state === "succeeded" && st.get(r.localCandidateId).candidateType === "relay") pair = r;
            if (!pair) await delay(100);
        }
        a.must(pair, "a relayed pair is selected after the restart");
        a.eq(st.get(pair.remoteCandidateId).candidateType, "relay");
        a.ok(types.length > 0 && types.every((x) => x === "relay"), "only relay candidates signalled after the restart: " + types.join(","));
        a.ok(!/typ (host|srflx)/.test(pa.localDescription.sdp), "the restart offer lists only relay candidates");
        a.eq(srv.stats.allocOk, 2);
        const got = waitEvent(rdc, "message", 5000);
        dc.send("via-relay");
        a.eq((await got).data, "via-relay");
        a.ok(srv.stats.sendInd + srv.stats.chanDataIn > 0, "data went through the TURN server");
    } finally { closeAll(pa, pb); await delay(50); srv.close(); }
}, { timeoutMs: 40000 });

t.test("stun: and turn: on the same server port: the STUN session does not swallow relay traffic (R2b-2)", async (a) => {
    const srv = new TurnServer();
    const both = [{ urls: "stun:127.0.0.1:" + srv.port }, { urls: "turn:127.0.0.1:" + srv.port, username: "user", credential: "pass" }];
    const pa = new RTCPeerConnection({ ...LO, iceServers: both }), pb = new RTCPeerConnection({ ...LO, iceServers: both });
    try {
        trickle(pa, pb);
        const dc = pa.createDataChannel("r");
        const remote = waitEvent(pb, "datachannel", 15000);
        remote.catch(() => {});
        await negotiate(pa, pb);
        await Promise.all([waitState(pa, "connected", 15000), waitState(pb, "connected", 15000)]);
        const rdc = (await remote).channel;
        if (dc.readyState !== "open") await waitEvent(dc, "open", 10000);
        a.ok(srv.stats.binding >= 2, "both peers ran a Binding against the server");
        const cfg = { ...LO, iceTransportPolicy: "relay", iceServers: both };
        pa.setConfiguration(cfg);
        pb.setConfiguration(cfg);
        pa.restartIce();
        await negotiate(pa, pb);
        let pair = null, st = null;
        for (let i = 0; i < 100 && !pair; i++) {
            st = await pa.getStats();
            for (const r of st.values())
                if (r.type === "candidate-pair" && r.nominated && r.state === "succeeded" && st.get(r.localCandidateId).candidateType === "relay") pair = r;
            if (!pair) await delay(100);
        }
        a.must(pair, "a relayed pair is selected although a STUN session shares the TURN server address");
        const got = waitEvent(rdc, "message", 5000);
        dc.send("via-relay");
        a.eq((await got).data, "via-relay");
        const back = waitEvent(dc, "message", 5000);
        rdc.send("echo");
        a.eq((await back).data, "echo");
        a.ok(srv.stats.dataInd + srv.stats.chanDataOut > 0, "relay data came back from the server");
    } finally { closeAll(pa, pb); await delay(50); srv.close(); }
}, { timeoutMs: 40000 });

t.test("setConfiguration iceLocalAddresses adds an interface at the ICE restart (R4-5 network change)", async (a) => {
    let v6 = true;
    try { const s = new UDPSocket({ port: 0, host: "::1" }); s.close(); } catch (e) { v6 = false; }
    if (!v6) a.skip("no IPv6 loopback");
    const pa = new RTCPeerConnection(LO), pb = new RTCPeerConnection(LO);
    try {
        trickle(pa, pb);
        const first = [];
        pa.addEventListener("icecandidate", (e) => { if (e.candidate && e.candidate.candidate) first.push(e.candidate.address); });
        pa.createDataChannel("g");
        await negotiate(pa, pb);
        await Promise.all([waitState(pa, "connected", 15000), waitState(pb, "connected", 15000)]);
        a.ok(first.length > 0 && first.every((x) => x === "127.0.0.1"), "before: " + first.join(","));
        pa.setConfiguration({ iceLocalAddresses: ["127.0.0.1", "::1"] });
        const g = gatherAll(pa);
        pa.restartIce();
        await negotiate(pa, pb);
        const second = (await g).cands.map((c) => c.address);
        a.ok(second.includes("::1"), "the new interface got a host candidate: " + second.join(","));
        a.ok(second.includes("127.0.0.1"), "the old one is signalled again");
        await waitState(pa, "connected", 15000);
    } finally { closeAll(pa, pb); }
});

t.test("relay -> all with the same TURN server: host and srflx candidates return at the restart, no second Allocate (R4-5)", async (a) => {
    const srv = new TurnServer({ mappedPortDelta: 1000 });
    const turn = [{ urls: "turn:127.0.0.1:" + srv.port, username: "user", credential: "pass" }];
    const relay = { ...LO, iceTransportPolicy: "relay", iceServers: turn };
    const pa = new RTCPeerConnection(relay), pb = new RTCPeerConnection(relay);
    try {
        trickle(pa, pb);
        pa.createDataChannel("r");
        const first = gatherAll(pa);
        await negotiate(pa, pb);
        a.ok((await first).cands.every((c) => c.type === "relay"), "relay only before");
        await Promise.all([waitState(pa, "connected", 15000), waitState(pb, "connected", 15000)]);
        a.eq(srv.stats.allocOk, 2);
        pa.setConfiguration({ ...LO, iceServers: turn });
        a.eq(pa.getConfiguration().iceTransportPolicy, "all");
        const g = gatherAll(pa);
        pa.restartIce();
        await negotiate(pa, pb);
        const types = (await g).cands.map((c) => c.type);
        a.ok(types.includes("host"), "a host candidate after switching to all: " + types.join(","));
        a.ok(types.includes("srflx"), "the srflx learned by the TURN session is signalled: " + types.join(","));
        a.ok(types.includes("relay"), "the relay candidate is signalled again: " + types.join(","));
        a.ok(/typ host/.test(pa.localDescription.sdp), "host candidate in the restart offer");
        await waitState(pa, "connected", 15000);
        await delay(200);
        a.eq(srv.stats.allocOk, 2, "the unchanged server keeps its allocation");
        a.eq(srv.stats.alloc401, 2, "no new Allocate challenge");
    } finally { closeAll(pa, pb); await delay(50); srv.close(); }
}, { timeoutMs: 40000 });

t.test("9 quick rounds of 8 servers: the round past 64 live sessions gets icecandidateerror naming the limit, never a silent skip (R4-5, DEC-a)", async (a) => {
    const srvs = [];
    for (let i = 0; i < 8; i++) srvs.push(new TurnServer());
    const cfg = (r) => ({ ...LO, iceServers: srvs.map((s) => ({ urls: "turn:127.0.0.1:" + s.port, username: "u" + r, credential: "pass" })) });
    const pa = new RTCPeerConnection(cfg(0)), pb = new RTCPeerConnection(LO);
    const codes = [], texts = [];
    pa.addEventListener("icecandidateerror", (e) => { codes.push(e.errorCode); texts.push(e.errorText); });
    try {
        pa.createDataChannel("c");
        for (let r = 0; r < 9; r++) {
            if (r) { pa.setConfiguration(cfg(r)); pa.restartIce(); }
            const g = gatherAll(pa, 15000);
            await negotiate(pa, pb);
            await g;
        }
        const n401 = codes.filter((c) => c === 401).length;
        a.eq(n401, 64, "8 rounds x 8 servers are tried; no candidates are exchanged, so no pair is ever selected and nothing is released");
        a.eq(codes.filter((c) => c === 701).length, 8, "the 9th round: one error per server: " + codes.join(","));
        a.ok(texts.filter((x) => /limit/.test(x)).length === 8, "errorText names the limit");
    } finally { closeAll(pa, pb); await delay(50); for (const s of srvs) s.close(); }
}, { timeoutMs: 60000 });

t.test("a server dropped by setConfiguration is released at the restart: Refresh 0 and its relay is not signalled again (R4-5)", async (a) => {
    const srv = new TurnServer();
    const cfg = { ...LO, iceServers: [{ urls: "turn:127.0.0.1:" + srv.port, username: "user", credential: "pass" }] };
    const pa = new RTCPeerConnection(cfg), pb = new RTCPeerConnection(LO);
    try {
        trickle(pa, pb);
        pa.createDataChannel("d");
        const first = gatherAll(pa);
        await negotiate(pa, pb);
        a.ok((await first).cands.some((c) => c.type === "relay"), "relay gathered from the server");
        await waitState(pa, "connected", 15000);
        a.eq(srv.stats.allocOk, 1);
        a.eq(srv.stats.dealloc, 0);
        pa.setConfiguration({ ...LO, iceServers: [] });
        a.eq(srv.stats.dealloc, 0, "nothing changes before the restart");
        const g = gatherAll(pa);
        pa.restartIce();
        await negotiate(pa, pb);
        const types = (await g).cands.map((c) => c.type);
        a.ok(!types.includes("relay"), "the dropped server's relay is not signalled again: " + types.join(","));
        a.ok(types.includes("host"), "host candidates are: " + types.join(","));
        a.ok(!/typ relay/.test(pa.localDescription.sdp), "no relay in the restart offer");
        for (let i = 0; i < 50 && !srv.stats.dealloc; i++) await delay(20);
        a.eq(srv.stats.dealloc, 1, "the allocation is released (Refresh LIFETIME 0, RFC 8656 7.3)");
        await waitState(pa, "connected", 15000);
        const st = await pa.getStats();
        const relayPairs = [...st.values()].filter((r) => r.type === "candidate-pair" && st.get(r.localCandidateId) && st.get(r.localCandidateId).candidateType === "relay");
        a.eq(relayPairs.length, 0, "the released relay is not paired after the restart");
    } finally { closeAll(pa, pb); await delay(50); srv.close(); }
}, { timeoutMs: 40000 });
t.test("iceTransportPolicy set by setConfiguration waits for the next gathering: a re-offer without restart keeps host candidates (R1b-9, JSEP 4.1.18)", async (a) => {
    const { a: x, b: y, dc, rdc } = await connectPair();
    x.setConfiguration({ ...LO, iceTransportPolicy: "relay" });
    a.eq(x.getConfiguration().iceTransportPolicy, "relay", "getConfiguration reports the new policy at once");
    const o = await x.createOffer();
    await x.setLocalDescription(o);
    a.ok(/typ host/.test(x.localDescription.sdp), "no restart yet: the local description still lists the gathered host candidate");
    await y.setRemoteDescription(o);
    const ans = await y.createAnswer();
    await y.setLocalDescription(ans);
    await x.setRemoteDescription(ans);
    await delay(300);
    const got = waitEvent(rdc, "message", 5000);
    dc.send("host pair still in use");
    a.eq((await got).data, "host pair still in use");
    a.eq(x.connectionState, "connected");
    closeAll(x, y);
});

t.test("a relayed connection moving to another TURN server keeps streaming over the old relay until the new pair is selected (DEC-a)", async (a) => {
    const s1 = new TurnServer(), s2 = new TurnServer();
    const turn = (s) => [{ urls: "turn:127.0.0.1:" + s.port, username: "user", credential: "pass" }];
    const pa = new RTCPeerConnection({ ...LO, iceTransportPolicy: "relay", iceServers: turn(s1) });
    const pb = new RTCPeerConnection({ ...LO, iceTransportPolicy: "relay", iceServers: turn(s1) });
    let timer = null;
    try {
        trickle(pa, pb);
        const dc = pa.createDataChannel("s");
        const remote = waitEvent(pb, "datachannel", 15000);
        remote.catch(() => {});
        await negotiate(pa, pb);
        await Promise.all([waitState(pa, "connected", 15000), waitState(pb, "connected", 15000)]);
        const rdc = (await remote).channel;
        if (dc.readyState !== "open") await waitEvent(dc, "open", 10000);
        let last = Date.now(), maxGap = 0, n = 0;
        rdc.onmessage = () => { const t = Date.now(); if (t - last > maxGap) maxGap = t - last; last = t; n++; };
        timer = setInterval(() => { if (dc.readyState === "open") dc.send("x"); }, 20);
        await delay(300);
        pa.setConfiguration({ ...LO, iceTransportPolicy: "relay", iceServers: turn(s2) });
        pa.restartIce();
        const o = await pa.createOffer();
        await pa.setLocalDescription(o);
        await pb.setRemoteDescription(o);
        await delay(1500);
        a.eq(s1.stats.dealloc, 0, "the old allocation is not released while its relay carries the selected pair");
        const ans = await pb.createAnswer();
        await pb.setLocalDescription(ans);
        await pa.setRemoteDescription(ans);
        let sel = null;
        for (let i = 0; i < 100 && !s1.stats.dealloc; i++) await delay(50);
        const st = await pa.getStats();
        for (const r of st.values()) if (r.type === "candidate-pair" && r.nominated && r.state === "succeeded") sel = r;
        a.eq(s1.stats.dealloc, 1, "released once the restart selected a pair (Refresh LIFETIME 0)");
        a.must(sel, "a pair is selected after the restart");
        a.eq(st.get(sel.localCandidateId).candidateType, "relay");
        a.ok(s2.stats.allocOk >= 1, "the new server allocated");
        await delay(300);
        a.ok(n > 50, "messages kept flowing: " + n);
        a.ok(maxGap < 1000, "no gap above 1 s while the restart was pending and while switching servers: " + maxGap + " ms");
        pa.setConfiguration({ ...LO, iceTransportPolicy: "relay", iceServers: turn(s1) });
        pa.restartIce();
        await negotiate(pa, pb);
        for (let i = 0; i < 100 && !s2.stats.dealloc; i++) await delay(50);
        a.eq(s2.stats.dealloc, 1, "back to the first server: the second one is released at the selection");
        const before = n;
        await delay(500);
        a.ok(n > before + 10, "the new session in the released session's slot carries the relayed pair: " + (n - before));
        a.ok(s1.stats.allocOk >= 3, "a fresh allocation on the first server: " + s1.stats.allocOk);
    } finally { clearInterval(timer); closeAll(pa, pb); await delay(50); s1.close(); s2.close(); }
}, { timeoutMs: 40000 });

t.test("sessions not yet released count toward 64; a server over the limit gets icecandidateerror, then released slots are reused (DEC-a)", async (a) => {
    const srvs = [];
    for (let i = 0; i < 8; i++) srvs.push(new TurnServer());
    const cfg = (r) => ({ ...LO, iceServers: srvs.map((s) => ({ urls: "turn:127.0.0.1:" + s.port, username: "u" + r, credential: "pass" })) });
    const pa = new RTCPeerConnection(cfg(0)), pb = new RTCPeerConnection(LO);
    const errs = [];
    pa.addEventListener("icecandidateerror", (e) => errs.push(e));
    try {
        trickle(pa, pb);
        pa.createDataChannel("c");
        let g = gatherAll(pa, 15000);
        await negotiate(pa, pb);
        await g;
        await waitState(pa, "connected", 15000);
        for (let r = 1; r <= 8; r++) {
            pa.setConfiguration(cfg(r));
            pa.restartIce();
            g = gatherAll(pa, 15000);
            await pa.setLocalDescription(await pa.createOffer());
            await g;
            await pa.setLocalDescription({ type: "rollback" });
        }
        const lim = errs.filter((e) => e.errorCode === 701);
        a.eq(lim.length, 8, "round 8 is over the 64 live sessions (8 current + 56 not yet released): " + errs.map((e) => e.errorCode).join(","));
        a.ok(lim.every((e) => /limit/.test(e.errorText)), "errorText names the limit: " + (lim.length && lim[0].errorText));
        a.eq(errs.filter((e) => e.errorCode === 401).length, 64, "rounds 0..7 were tried");
        pa.setConfiguration(cfg(9));
        g = gatherAll(pa, 15000);
        await negotiate(pa, pb);
        await g;
        for (let i = 0; i < 50; i++) { const st = await pa.getStats(); let s = false; for (const x of st.values()) if (x.type === "candidate-pair" && x.nominated && x.state === "succeeded") s = true; if (s) break; await delay(50); }
        await delay(200);
        errs.length = 0;
        pa.setConfiguration(cfg(10));
        pa.restartIce();
        g = gatherAll(pa, 15000);
        await negotiate(pa, pb);
        await g;
        a.eq(errs.filter((e) => e.errorCode === 701).length, 0, "after the restart selected a pair, the released sessions' slots are reused: " + errs.map((e) => e.errorCode).join(","));
        a.eq(errs.filter((e) => e.errorCode === 401).length, 8, "all 8 servers of the next round are tried");
    } finally { closeAll(pa, pb); await delay(50); for (const s of srvs) s.close(); }
}, { timeoutMs: 60000 });


t.test("a remote ICE restart answered while a silent STUN server is still being gathered never reports completed before gathering ends (round 3)", async (a) => {
    const silent = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const { a: pa, b: pb } = await connectPair();
    try {
        const log = [];
        pb.addEventListener("iceconnectionstatechange", () => log.push(pb.iceConnectionState + "/" + pb.iceGatheringState));
        pb.setConfiguration({ ...LO, iceServers: [{ urls: "stun:127.0.0.1:" + silent.port }] });
        const eoc = new Promise((res) => pa.addEventListener("icecandidate", (e) => { if (!e.candidate) res(); }));
        pa.restartIce();
        await negotiate(pa, pb);
        await eoc;
        await delay(2500);
        a.eq(pb.iceGatheringState, "gathering", "the silent STUN server keeps the answerer gathering");
        a.ok(["connected", "checking"].includes(pb.iceConnectionState), "the answerer is not completed while gathering: " + pb.iceConnectionState);
        a.ok(!log.some((s) => s.startsWith("completed/gathering")), "no completed while gathering: " + log.join(","));
        a.ok(log.includes("connected/gathering") || pb.iceConnectionState === "connected", "the restarted pair connects: " + log.join(","));
    } finally { closeAll(pa, pb); silent.close(); }
}, { timeoutMs: 20000 });

await t.run();
