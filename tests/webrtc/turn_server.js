// turn_server.js -- a small STUN (RFC 8489) + TURN-over-UDP (RFC 8656) server for the dyna:webrtc suites, written
// independently of the C client so it acts as an oracle: Binding, Allocate with the long-term credential mechanism
// (401 + REALM/NONCE, then MESSAGE-INTEGRITY over MD5(user:realm:pass)), Refresh (LIFETIME 0 deallocates),
// CreatePermission, ChannelBind, Send/Data indications and ChannelData. Relay sockets bind 127.0.0.1.
// `mappedPortDelta` makes Binding/Allocate report a mapped port shifted from the real one, so a loopback client
// sees a server-reflexive address that differs from its host candidate (a NAT stand-in).
import { UDPSocket } from "dyna:net";
import { HMAC, MD5, CRC32, RandomBytes } from "dyna:crypto";

const MAGIC = 0x2112a442;
const T = { BIND: 0x001, ALLOC: 0x003, REFRESH: 0x004, SEND: 0x006, DATA: 0x007, PERM: 0x008, CHAN: 0x009 };
const A = { USERNAME: 0x0006, MI: 0x0008, ERR: 0x0009, CHANNEL: 0x000c, LIFETIME: 0x000d, XPEER: 0x0012, DATA: 0x0013,
    REALM: 0x0014, NONCE: 0x0015, XRELAY: 0x0016, TRANSPORT: 0x0019, XMAPPED: 0x0020, FP: 0x8028 };

function u8(x) { return x instanceof Uint8Array ? x : new Uint8Array(x); }
function rd16(b, o) { return (b[o] << 8) | b[o + 1]; }
function rd32(b, o) { return ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0; }

function parse(b) {
    if (b.length < 20 || (b[0] & 0xc0) || rd32(b, 4) !== MAGIC) return null;
    const len = rd16(b, 2);
    if (len + 20 > b.length || (len & 3)) return null;
    const m = { type: rd16(b, 0), tid: b.slice(8, 20), attrs: [], raw: b.subarray(0, 20 + len) };
    let o = 20;
    while (o + 4 <= 20 + len) {
        const t = rd16(b, o), l = rd16(b, o + 2);
        if (o + 4 + l > 20 + len) return null;
        m.attrs.push({ t, v: b.subarray(o + 4, o + 4 + l), off: o });
        o += 4 + ((l + 3) & ~3);
    }
    return m;
}
function attr(m, t) { const a = m.attrs.find((x) => x.t === t); return a ? a.v : null; }
function method(type) { return (type & 0x000f) | ((type & 0x00e0) >> 1) | ((type & 0x3e00) >> 2); }
function cls(type) { return type & 0x0110; }
function mktype(meth, c) { return (meth & 0x000f) | ((meth & 0x0070) << 1) | ((meth & 0x0f80) << 2) | c; }

function xaddr(addr, port) {
    const ip = addr.split(".").map(Number);
    const v = new Uint8Array(8);
    v[1] = 1;
    v[2] = (port ^ (MAGIC >>> 16)) >> 8; v[3] = (port ^ (MAGIC >>> 16)) & 255;
    const mk = [0x21, 0x12, 0xa4, 0x42];
    for (let i = 0; i < 4; i++) v[4 + i] = ip[i] ^ mk[i];
    return v;
}
function unxaddr(v) {
    if (!v || v.length < 8 || v[1] !== 1) return null;
    const port = rd16(v, 2) ^ (MAGIC >>> 16);
    const mk = [0x21, 0x12, 0xa4, 0x42];
    return { address: [0, 1, 2, 3].map((i) => v[4 + i] ^ mk[i]).join("."), port };
}

export class Writer {
    constructor(type, tid) { this.b = []; this.type = type; this.tid = tid; }
    add(t, v) { this.b.push({ t, v: u8(v) }); return this; }
    u32(t, x) { return this.add(t, [x >>> 24, (x >>> 16) & 255, (x >>> 8) & 255, x & 255]); }
    err(code, reason) { const r = new TextEncoder().encode(reason); const v = new Uint8Array(4 + r.length); v[2] = code / 100 | 0; v[3] = code % 100; v.set(r, 4); return this.add(A.ERR, v); }
    build(key, fp) {
        let len = 0;
        for (const a of this.b) len += 4 + ((a.v.length + 3) & ~3);
        const total = 20 + len + (key ? 24 : 0) + (fp ? 8 : 0);
        const out = new Uint8Array(total);
        out[0] = this.type >> 8; out[1] = this.type & 255;
        out[4] = 0x21; out[5] = 0x12; out[6] = 0xa4; out[7] = 0x42;
        out.set(this.tid, 8);
        let o = 20;
        for (const a of this.b) {
            out[o] = a.t >> 8; out[o + 1] = a.t & 255; out[o + 2] = a.v.length >> 8; out[o + 3] = a.v.length & 255;
            out.set(a.v, o + 4);
            o += 4 + ((a.v.length + 3) & ~3);
        }
        if (key) {
            const l = o - 20 + 24;
            out[2] = l >> 8; out[3] = l & 255;
            const mac = u8(HMAC("sha1", key, out.subarray(0, o)));
            out[o] = 0; out[o + 1] = 8; out[o + 2] = 0; out[o + 3] = 20;
            out.set(mac, o + 4);
            o += 24;
        }
        if (fp) {
            const l = o - 20 + 8;
            out[2] = l >> 8; out[3] = l & 255;
            const c = (CRC32(out.subarray(0, o)) ^ 0x5354554e) >>> 0;
            out[o] = 0x80; out[o + 1] = 0x28; out[o + 2] = 0; out[o + 3] = 4;
            out[o + 4] = c >>> 24; out[o + 5] = (c >>> 16) & 255; out[o + 6] = (c >>> 8) & 255; out[o + 7] = c & 255;
            o += 8;
        }
        const l = o - 20;
        out[2] = l >> 8; out[3] = l & 255;
        return out;
    }
}

function checkMI(m, key) {
    const a = m.attrs.find((x) => x.t === A.MI);
    if (!a) return false;
    const pre = m.raw.slice(0, a.off);
    const l = a.off - 20 + 24;
    pre[2] = l >> 8; pre[3] = l & 255;
    const mac = u8(HMAC("sha1", key, pre));
    for (let i = 0; i < 20; i++) if (mac[i] !== a.v[i]) return false;
    return true;
}

export class TurnServer {
    constructor({ user = "user", pass = "pass", realm = "dyna.test", mappedPortDelta = 0, lifetime = 600 } = {}) {
        this.user = user; this.pass = pass; this.realm = realm; this.delta = mappedPortDelta; this.lifetime = lifetime;
        this.nonce = "n" + Math.floor(Math.random() * 1e9).toString(16);
        this.key = u8(MD5(user + ":" + realm + ":" + pass));
        this.allocs = new Map();
        this.stats = { binding: 0, allocOk: 0, alloc401: 0, refresh: 0, dealloc: 0, perm: 0, chan: 0, sendInd: 0, chanDataIn: 0, dataInd: 0, chanDataOut: 0, badMI: 0 };
        this.sock = new UDPSocket({ port: 0, host: "127.0.0.1" });
        this.port = this.sock.port;
        this.sock.start({ message: (d, from) => this.onPacket(u8(d), from) });
    }
    k(from) { return from.address + ":" + from.port; }
    send(b, to) { try { this.sock.send(b, to.address, to.port); } catch (e) {} }
    onPacket(b, from) {
        if (b.length >= 4 && b[0] >= 0x40 && b[0] <= 0x7f) {
            const al = this.allocs.get(this.k(from));
            const ch = rd16(b, 0), len = rd16(b, 2);
            if (!al || len + 4 > b.length) return;
            const peer = al.chans.get(ch);
            if (!peer) return;
            this.stats.chanDataIn++;
            try { al.relay.send(b.subarray(4, 4 + len), peer.address, peer.port); } catch (e) {}
            return;
        }
        const m = parse(b);
        if (!m) return;
        const meth = method(m.type), c = cls(m.type);
        if (meth === T.BIND && c === 0) {
            this.stats.binding++;
            this.send(new Writer(mktype(T.BIND, 0x100), m.tid).add(A.XMAPPED, xaddr(from.address, from.port + this.delta)).build(null, true), from);
            return;
        }
        if (meth === T.SEND && c === 0x10) {
            const al = this.allocs.get(this.k(from));
            const p = unxaddr(attr(m, A.XPEER)), d = attr(m, A.DATA);
            if (!al || !p || !d || !al.perms.has(p.address)) return;
            this.stats.sendInd++;
            try { al.relay.send(d, p.address, p.port); } catch (e) {}
            return;
        }
        if (c !== 0) return;
        const uname = attr(m, A.USERNAME), nonce = attr(m, A.NONCE);
        const fail = (code, reason) => this.send(new Writer(mktype(meth, 0x110), m.tid).err(code, reason)
            .add(A.REALM, new TextEncoder().encode(this.realm)).add(A.NONCE, new TextEncoder().encode(this.nonce)).build(null, true), from);
        if (!uname || !attr(m, A.MI)) { if (meth === T.ALLOC) this.stats.alloc401++; return fail(401, "Unauthorized"); }
        if (new TextDecoder().decode(uname) !== this.user || !checkMI(m, this.key)) { this.stats.badMI++; return fail(401, "Unauthorized"); }
        if (!nonce || new TextDecoder().decode(nonce) !== this.nonce) return fail(438, "Stale Nonce");
        const ok = () => new Writer(mktype(meth, 0x100), m.tid);
        const key = this.k(from);
        let al = this.allocs.get(key);
        if (meth === T.ALLOC) {
            if (al) return this.send(new Writer(mktype(meth, 0x110), m.tid).err(437, "Allocation Mismatch").build(this.key, true), from);
            const relay = new UDPSocket({ port: 0, host: "127.0.0.1" });
            al = { relay, perms: new Set(), chans: new Map(), peers: new Map(), client: from };
            relay.start({ message: (d, src) => this.fromPeer(al, u8(d), src) });
            this.allocs.set(key, al);
            this.stats.allocOk++;
            return this.send(ok().add(A.XRELAY, xaddr("127.0.0.1", relay.port)).add(A.XMAPPED, xaddr(from.address, from.port + this.delta))
                .u32(A.LIFETIME, this.lifetime).build(this.key, true), from);
        }
        if (!al) return this.send(new Writer(mktype(meth, 0x110), m.tid).err(437, "Allocation Mismatch").build(this.key, true), from);
        if (meth === T.REFRESH) {
            const lt = attr(m, A.LIFETIME);
            const life = lt ? rd32(lt, 0) : this.lifetime;
            this.stats.refresh++;
            if (life === 0) { al.relay.close(); this.allocs.delete(key); this.stats.dealloc++; }
            return this.send(ok().u32(A.LIFETIME, life ? this.lifetime : 0).build(this.key, true), from);
        }
        if (meth === T.PERM) {
            for (const a of m.attrs) if (a.t === A.XPEER) { const p = unxaddr(a.v); if (p) al.perms.add(p.address); }
            this.stats.perm++;
            return this.send(ok().build(this.key, true), from);
        }
        if (meth === T.CHAN) {
            const cn = attr(m, A.CHANNEL), p = unxaddr(attr(m, A.XPEER));
            if (!cn || !p) return this.send(new Writer(mktype(meth, 0x110), m.tid).err(400, "Bad Request").build(this.key, true), from);
            const ch = rd16(cn, 0);
            if (ch < 0x4000 || ch > 0x4fff) return this.send(new Writer(mktype(meth, 0x110), m.tid).err(400, "Bad Request").build(this.key, true), from);
            al.chans.set(ch, p);
            al.peers.set(p.address + ":" + p.port, ch);
            al.perms.add(p.address);
            this.stats.chan++;
            return this.send(ok().build(this.key, true), from);
        }
    }
    fromPeer(al, d, src) {
        if (!al.perms.has(src.address)) return;
        const ch = al.peers.get(src.address + ":" + src.port);
        if (ch !== undefined) {
            const out = new Uint8Array(4 + ((d.length + 3) & ~3));
            out[0] = ch >> 8; out[1] = ch & 255; out[2] = d.length >> 8; out[3] = d.length & 255;
            out.set(d, 4);
            this.stats.chanDataOut++;
            return this.send(out, al.client);
        }
        this.stats.dataInd++;
        const w = new Writer(mktype(T.DATA, 0x10), u8(RandomBytes(12))).add(A.XPEER, xaddr(src.address, src.port)).add(A.DATA, d);
        this.send(w.build(null, false), al.client);
    }
    close() { for (const al of this.allocs.values()) al.relay.close(); this.allocs.clear(); this.sock.close(); }
}
