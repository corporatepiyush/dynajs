// rtc_util.js -- shared fixtures for the dyna:webrtc suites: wait for an event with a deadline, wire two
// RTCPeerConnections in one process (trickle or full-SDP), and a lossy UDP relay that sits between them by
// rewriting each host candidate to a relay port, so loss, duplication and reordering hit every DTLS/SCTP packet.
import { UDPSocket } from "dyna:net";

export const LO = Object.freeze({ iceLocalAddresses: ["127.0.0.1"] });

export function waitEvent(target, type, ms = 10000, pred) {
    return new Promise((res, rej) => {
        const t = setTimeout(() => { target.removeEventListener(type, h); rej(new Error("timeout waiting for " + type)); }, ms);
        function h(e) {
            if (pred && !pred(e)) return;
            clearTimeout(t);
            target.removeEventListener(type, h);
            res(e);
        }
        target.addEventListener(type, h);
    });
}

export function waitState(pc, want, ms = 10000) {
    if (pc.connectionState === want) return Promise.resolve();
    return waitEvent(pc, "connectionstatechange", ms, () => pc.connectionState === want).then(() => undefined);
}

export function delay(ms) { return new Promise((r) => setTimeout(r, ms)); }

export function trickle(a, b, rewrite) {
    const fwd = (from, to) => (e) => {
        if (!e.candidate) { to.addIceCandidate(null).catch(() => {}); return; }
        let init = e.candidate.toJSON();
        if (rewrite) init = rewrite(from, init);
        if (init) to.addIceCandidate(init).catch(() => {});
    };
    a.addEventListener("icecandidate", fwd(a, b));
    b.addEventListener("icecandidate", fwd(b, a));
}

export async function negotiate(a, b) {
    const o = await a.createOffer();
    await a.setLocalDescription(o);
    await b.setRemoteDescription(o);
    const ans = await b.createAnswer();
    await b.setLocalDescription(ans);
    await a.setRemoteDescription(ans);
}

export async function connectPair(opts = {}) {
    const a = new RTCPeerConnection(opts.configA || LO), b = new RTCPeerConnection(opts.configB || LO);
    trickle(a, b, opts.rewrite);
    const dc = opts.noChannel ? null : a.createDataChannel(opts.label || "t", opts.init);
    const remote = dc && !(opts.init && opts.init.negotiated) ? waitEvent(b, "datachannel", opts.ms || 10000) : null;
    if (remote) remote.catch(() => {});
    await negotiate(a, b);
    try {
        await Promise.all([waitState(a, "connected", opts.ms), waitState(b, "connected", opts.ms)]);
    } catch (e) {
        const st = (p) => p.signalingState + "/" + p.iceConnectionState + "/" + p.connectionState + "/" + p.iceGatheringState;
        const msg = e.message + " a=" + st(a) + " b=" + st(b);
        a.close();
        b.close();
        throw new Error(msg);
    }
    let rdc = null;
    if (remote) rdc = (await remote).channel;
    if (dc && dc.readyState !== "open") await waitEvent(dc, "open", opts.ms || 10000);
    if (rdc && rdc.readyState !== "open") await waitEvent(rdc, "open", opts.ms || 10000);
    return { a, b, dc, rdc };
}

export function closeAll(...pcs) { for (const p of pcs) if (p) p.close(); }

export function lcg(seed) {
    let s = seed >>> 0;
    return () => ((s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296);
}

export class LossyRelay {
    constructor({ loss = 0, dup = 0, reorder = 0, seed = 1 } = {}) {
        this.loss = loss; this.dup = dup; this.reorder = reorder;
        this.rnd = lcg(seed);
        this.stats = { fwd: 0, dropped: 0, duped: 0, reordered: 0 };
        this.sides = new Map();
    }
    side(pc) {
        let s = this.sides.get(pc);
        if (!s) {
            const sock = new UDPSocket({ port: 0, host: "127.0.0.1" });
            s = { sock, target: null, other: null };
            this.sides.set(pc, s);
        }
        return s;
    }
    pair(a, b) {
        const sa = this.side(a), sb = this.side(b);
        sa.other = sb; sb.other = sa;
        sa.sock.start({ message: (data) => this.deliver(sb.sock, data, sa.target) });
        sb.sock.start({ message: (data) => this.deliver(sa.sock, data, sb.target) });
    }
    rewrite() {
        return (from, init) => {
            const s = this.sides.get(from);
            const m = /^(candidate:\S+ \d+ udp \d+ )(\S+) (\d+)( typ host.*)$/.exec(init.candidate || "");
            if (!m || !s) return null;
            s.target = s.target || { address: m[2], port: +m[3] };
            return Object.assign({}, init, { candidate: m[1] + "127.0.0.1 " + s.sock.port + m[4] });
        };
    }
    deliver(sock, data, to) {
        if (!to) return;
        if (this.rnd() < this.loss) { this.stats.dropped++; return; }
        const copies = this.rnd() < this.dup ? 2 : 1;
        if (copies === 2) this.stats.duped++;
        for (let i = 0; i < copies; i++) {
            if (this.rnd() < this.reorder) {
                this.stats.reordered++;
                const d = data.slice();
                setTimeout(() => { try { sock.send(d, to.address, to.port); } catch (e) {} }, 1 + Math.floor(this.rnd() * 20));
                continue;
            }
            this.stats.fwd++;
            try { sock.send(data, to.address, to.port); } catch (e) {}
        }
    }
    close() { for (const s of this.sides.values()) s.sock.close(); }
}
