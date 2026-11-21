// A scriptable SSH-2 server written in JS over dyna:net TCPServer: curve25519-sha256, aes256-gcm@openssh.com, an
// ssh-ed25519 host key, all through dyna:crypto/dyna:hash. It exists to drive the dyna:ssh client into states a real
// sshd never produces: weak-only or non-strict KEXINIT, a forged host signature, wrong first packets, and fuzzed
// packets both in the cleartext phase and after NEWKEYS (encrypted, so the client's post-KEX parsers are reached).
import { TCPServer } from "dyna:net";
import { SHA256 } from "dyna:hash";
import { X25519Generate, X25519Derive, Ed25519Generate, Ed25519Sign, AESGCM, RandomBytes } from "dyna:crypto";

const te = new TextEncoder();
const td = new TextDecoder();

export class W {
    constructor() { this.parts = []; this.n = 0; }
    raw(u8) { this.parts.push(u8); this.n += u8.byteLength; return this; }
    u8(v) { return this.raw(Uint8Array.of(v & 255)); }
    u32(v) { return this.raw(Uint8Array.of((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255)); }
    str(x) { const b = typeof x === "string" ? te.encode(x) : x; this.u32(b.byteLength); return this.raw(b); }
    bool(v) { return this.u8(v ? 1 : 0); }
    mpint(b) {
        let i = 0;
        while (i < b.length && b[i] === 0) i++;
        b = b.subarray(i);
        if (b.length && (b[0] & 0x80)) { this.u32(b.length + 1); this.u8(0); return this.raw(b); }
        return this.str(b);
    }
    done() { const o = new Uint8Array(this.n); let k = 0; for (const p of this.parts) { o.set(p, k); k += p.byteLength; } return o; }
}

export class R {
    constructor(u8, off = 0) { this.b = u8; this.o = off; }
    u8() { return this.b[this.o++]; }
    u32() { const b = this.b, o = this.o; this.o += 4; return ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0; }
    str() { const n = this.u32(); const s = this.b.subarray(this.o, this.o + n); this.o += n; return s; }
    cstr() { return td.decode(this.str()); }
    bool() { return this.u8() !== 0; }
}

const cat = (...a) => { const w = new W(); for (const x of a) w.raw(x); return w.done(); };

export function b64(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return btoa(s);
}

export const DEFAULT_KEX = "curve25519-sha256,ext-info-s,kex-strict-s-v00@openssh.com";

export class FakeSSH {
    constructor(opts = {}) {
        this.o = Object.assign({
            version: "SSH-2.0-FakeSSH_1.0",
            kex: DEFAULT_KEX,
            hostKeyAlgs: "ssh-ed25519",
            ciphers: "aes256-gcm@openssh.com",
            comp: "none",
        }, opts);
        this.host = Ed25519Generate();
        this.hostType = this.o.hostKind === "sk" ? "sk-ssh-ed25519@openssh.com" : "ssh-ed25519";
        this.hostBlob = this.o.hostKind === "sk" ? new W().str(this.hostType).str(this.host.publicKey).str("ssh:").done() : new W().str("ssh-ed25519").str(this.host.publicKey).done();
        this.sessions = [];
        this.server = new TCPServer({ port: 0 });
        this.server.start({
            connect: (c) => { if (!c) return; try { this.sessions.push(new Session(this, c)); } catch (e) { console.log("fake server error: " + e.message + "\n" + e.stack); } },
            data: (c, b) => { const s = this.sessions.find((x) => x.c === c); if (!s) return; try { s.input(b); } catch (e) { s.error = e; if (this.o.onError) this.o.onError(e); else console.log("fake server error: " + e.message + "\n" + e.stack); s.end(); } },
            close: (c) => { const s = this.sessions.find((x) => x.c === c); if (s) s.closed = true; },
        });
        this.port = this.server.port;
    }
    knownHostsLine() { return "[127.0.0.1]:" + this.port + " " + this.hostType + " " + b64(this.hostBlob) + "\n"; }
    close() { for (const s of this.sessions) { try { s.c.close(); } catch {} } this.server.close(); }
}

class Session {
    constructor(srv, c) {
        this.srv = srv;
        this.o = srv.o;
        this.c = c;
        this.buf = new Uint8Array(0);
        this.vc = null;
        this.out = null;
        this.inn = null;
        this.closed = false;
        this.sid = null;
        this.chans = [];
        this.log = [];
        if (this.o.greeting !== undefined) c.write(this.o.greeting);
        else c.write(this.o.version + "\r\n");
        if (this.o.onGreeted) this.o.onGreeted(this);
        else this.sendKexinit();
    }
    kexinitPayload() {
        const w = new W().u8(20).raw(RandomBytes(16));
        w.str(this.o.kex).str(this.o.hostKeyAlgs).str(this.o.ciphers).str(this.o.ciphers).str("").str("")
            .str(this.o.comp).str(this.o.comp).str("").str("").bool(false).u32(0);
        return w.done();
    }
    sendKexinit() { this.is = this.kexinitPayload(); this.send(this.is); }
    packet(payload) {
        const bs = this.out ? 16 : 8, aad = this.out ? 4 : 0;
        let pad = bs - ((5 + payload.length - aad) % bs);
        if (pad < 4) pad += bs;
        const plen = 1 + payload.length + pad;
        const body = cat(Uint8Array.of(pad), payload, new Uint8Array(pad));
        const len = new W().u32(plen).done();
        if (!this.out) return cat(len, body);
        const ct = this.out.g.seal(this.out.iv, body, len);
        inc(this.out.iv);
        return cat(len, ct);
    }
    send(payload) { if (!this.closed) this.c.write(this.packet(payload)); }
    sendRaw(bytes) { if (!this.closed) this.c.write(bytes); }
    end() { try { this.c.close(); } catch {} this.closed = true; }
    input(b) {
        this.buf = cat(this.buf, b);
        for (;;) {
            if (this.vc === null) {
                const i = this.buf.indexOf(10);
                if (i < 0) return;
                this.vc = td.decode(this.buf.subarray(0, i)).replace(/\r$/, "");
                this.buf = this.buf.subarray(i + 1);
                continue;
            }
            if (this.buf.length < 4) return;
            const L = new R(this.buf).u32();
            const tag = this.inn ? 16 : 0;
            if (this.buf.length < 4 + L + tag) return;
            let body;
            if (this.inn) {
                body = this.inn.g.open(this.inn.iv, this.buf.subarray(4, 4 + L + 16), this.buf.subarray(0, 4));
                inc(this.inn.iv);
            } else body = this.buf.subarray(4, 4 + L);
            this.buf = this.buf.subarray(4 + L + tag);
            const pad = body[0];
            this.handle(body.subarray(1, L - pad));
            if (this.closed) return;
        }
    }
    handle(p) {
        const t = p[0], r = new R(p, 1);
        this.log.push(t);
        if (this.o.onPacket && this.o.onPacket(this, t, p) === true) return;
        if (t === 20) { this.ic = p.slice(); return; }
        if (t === 30) return this.kexReply(r.str());
        if (t === 21) { this.inn = this.pendIn; return; }
        if (t === 5) return this.send(new W().u8(6).str("ssh-userauth").done());
        if (t === 50) {
            if (this.o.authFail) return this.send(new W().u8(51).str("publickey").bool(false).done());
            if (this.o.banner) this.send(new W().u8(53).str(this.o.banner).str("").done());
            return this.send(new W().u8(52).done());
        }
        if (t === 90) {
            r.str();
            const sender = r.u32();
            const id = this.chans.length;
            this.chans.push({ rid: sender, id });
            if (this.o.refuseChannel) return this.send(new W().u8(92).u32(sender).u32(1).str("nope").str("").done());
            return this.send(new W().u8(91).u32(sender).u32(id).u32(this.o.window ?? 2097152).u32(32768).done());
        }
        if (t === 98) {
            const id = r.u32(), ty = r.cstr(), want = r.bool();
            const ch = this.chans[id];
            if (ty === "exec") {
                ch.cmd = r.cstr();
                if (want) this.send(new W().u8(99).u32(ch.rid).done());
                if (this.o.onExec) this.o.onExec(this, ch);
            }
            return;
        }
    }
    kexReply(qc) {
        const x = X25519Generate();
        const k = new W().mpint(X25519Derive(x.privateKey, qc)).done();
        const ks = this.srv.hostBlob;
        const qs = x.publicKey;
        const h = SHA256(new W().str(this.vc).str(this.o.version).str(this.ic).str(this.is).str(ks).str(qc).str(qs).raw(k).done());
        if (!this.sid) this.sid = h;
        const sk = this.o.hostKind === "sk";
        let sig = Ed25519Sign(this.srv.host.privateKey, sk ? cat(SHA256(new TextEncoder().encode("ssh:")), Uint8Array.of(1, 0, 0, 0, 7), SHA256(h)) : h);
        if (this.o.badSig) { sig = sig.slice(); sig[5] ^= 1; }
        const sigBlob = sk ? new W().str(this.srv.hostType).str(sig).u8(1).u32(7).done() : new W().str("ssh-ed25519").str(sig).done();
        this.send(new W().u8(31).str(this.o.replyHostBlob || ks).str(qs).str(sigBlob).done());
        const d = (letter, n) => {
            let out = SHA256(cat(new W().raw(k).done(), h, Uint8Array.of(letter.charCodeAt(0)), this.sid));
            while (out.length < n) out = cat(out, SHA256(cat(k, h, out)));
            return out.subarray(0, n);
        };
        this.send(Uint8Array.of(21));
        this.out = { g: new AESGCM(d("D", 32)), iv: d("B", 12).slice() };
        this.pendIn = { g: new AESGCM(d("C", 32)), iv: d("A", 12).slice() };
    }
}

function inc(iv) {
    for (let i = 11; i >= 4; i--) { iv[i] = (iv[i] + 1) & 255; if (iv[i] !== 0) break; }
}
