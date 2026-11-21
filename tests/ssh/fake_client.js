// A minimal SSH-2 client written in JS over dyna:net (curve25519-sha256, aes256-gcm@openssh.com, strict KEX) that can send
// publickey USERAUTH_REQUESTs the dyna:ssh client never produces: a signature by another key, a corrupted signature, and
// sk-ssh-ed25519@openssh.com signatures with or without the user-presence flag. auth() resolves with the server's reply type
// (51 failure, 52 success) or "closed".
import { TCPServer } from "dyna:net";
import { SHA256 } from "dyna:hash";
import { X25519Generate, X25519Derive, Ed25519Sign, AESGCM, RandomBytes } from "dyna:crypto";
import { W, R } from "./fake_server.js";

const te = new TextEncoder();
const td = new TextDecoder();
const cat = (...a) => { const w = new W(); for (const x of a) w.raw(x); return w.done(); };
function inc(iv) { for (let i = 11; i >= 4; i--) { iv[i] = (iv[i] + 1) & 255; if (iv[i] !== 0) break; } }

export class FakeClient {
    constructor(port) {
        this.port = port;
        this.buf = new Uint8Array(0);
        this.vs = null;
        this.q = [];
        this.wait = null;
        this.out = null;
        this.inn = null;
        this.closed = false;
        this.vc = "SSH-2.0-FakeClient_1.0";
    }
    open() {
        return new Promise((res, rej) => {
            this.cl = TCPServer.connect({ host: "127.0.0.1", port: this.port }, {
                connect: (u) => { if (!u) { rej(new Error("connect failed")); return; } this.c = u; u.write(this.vc + "\r\n"); res(); },
                data: (u, b) => this.input(b),
                close: () => { this.closed = true; this.flush(); },
            });
        });
    }
    flush() { if (this.wait && (this.q.length || this.closed)) { const w = this.wait; this.wait = null; w(); } }
    next() { return new Promise((res) => { this.wait = res; this.flush(); }).then(() => this.q.length ? this.q.shift() : null); }
    async expect(...types) { for (;;) { const p = await this.next(); if (p === null) return null; if (types.includes(p[0])) return p; } }
    input(b) {
        this.buf = cat(this.buf, b);
        for (;;) {
            if (this.needKeys) break;
            if (this.vs === null) {
                const i = this.buf.indexOf(10);
                if (i < 0) break;
                this.vs = td.decode(this.buf.subarray(0, i)).replace(/\r$/, "");
                this.buf = this.buf.subarray(i + 1);
                continue;
            }
            if (this.buf.length < 4) break;
            const L = new R(this.buf).u32(), tag = this.inn ? 16 : 0;
            if (this.buf.length < 4 + L + tag) break;
            let body;
            if (this.inn) { body = this.inn.g.open(this.inn.iv, this.buf.subarray(4, 4 + L + 16), this.buf.subarray(0, 4)); inc(this.inn.iv); }
            else body = this.buf.subarray(4, 4 + L);
            this.buf = this.buf.subarray(4 + L + tag);
            const p = body.slice(1, L - body[0]);
            this.q.push(p);
            if (p[0] === 21) { this.needKeys = true; break; }
        }
        this.flush();
    }
    send(payload) {
        const bs = this.out ? 16 : 8, aad = this.out ? 4 : 0;
        let pad = bs - ((5 + payload.length - aad) % bs);
        if (pad < 4) pad += bs;
        const plen = 1 + payload.length + pad;
        const body = cat(Uint8Array.of(pad), payload, RandomBytes(pad));
        const len = new W().u32(plen).done();
        if (!this.out) { this.c.write(cat(len, body)); return; }
        const ct = this.out.g.seal(this.out.iv, body, len);
        inc(this.out.iv);
        this.c.write(cat(len, ct));
    }
    async handshake() {
        await this.open();
        this.ic = new W().u8(20).raw(RandomBytes(16)).str("curve25519-sha256,ext-info-c,kex-strict-c-v00@openssh.com").str("ssh-ed25519")
            .str("aes256-gcm@openssh.com").str("aes256-gcm@openssh.com").str("").str("").str("none").str("none").str("").str("").bool(false).u32(0).done();
        this.send(this.ic);
        const is = await this.expect(20);
        const x = X25519Generate();
        this.send(new W().u8(30).str(x.publicKey).done());
        const rep = await this.expect(31);
        const r = new R(rep, 1);
        const ks = r.str(), qs = r.str();
        const k = new W().mpint(X25519Derive(x.privateKey, qs)).done();
        const h = SHA256(new W().str(this.vc).str(this.vs).str(this.ic).str(is.slice()).str(ks).str(x.publicKey).str(qs).raw(k).done());
        this.sid = h;
        const d = (letter, n) => {
            let out = SHA256(cat(k, h, Uint8Array.of(letter.charCodeAt(0)), h));
            while (out.length < n) out = cat(out, SHA256(cat(k, h, out)));
            return out.subarray(0, n);
        };
        const keysIn = { g: new AESGCM(d("D", 32)), iv: d("B", 12).slice() };
        await this.expect(21);
        this.inn = keysIn;
        this.needKeys = false;
        this.input(new Uint8Array(0));
        this.send(Uint8Array.of(21));
        this.out = { g: new AESGCM(d("C", 32)), iv: d("A", 12).slice() };
        this.send(new W().u8(5).str("ssh-userauth").done());
        await this.expect(6);
    }
    async auth(user, o) {
        const blob = o.sk ? new W().str("sk-ssh-ed25519@openssh.com").str(o.publicKey).str("ssh:").done() : new W().str("ssh-ed25519").str(o.publicKey).done();
        const alg = o.sk ? "sk-ssh-ed25519@openssh.com" : "ssh-ed25519";
        const data = new W().str(this.sid).u8(50).str(user).str("ssh-connection").str("publickey").bool(true).str(alg).str(blob).done();
        let sig;
        if (o.sk) sig = Ed25519Sign(o.signWith, cat(SHA256(te.encode("ssh:")), Uint8Array.of(o.flags, 0, 0, 0, 1), SHA256(data)));
        else sig = Ed25519Sign(o.signWith, data);
        if (o.corrupt) { sig = sig.slice(); sig[9] ^= 4; }
        const sb = o.sk ? new W().str(alg).str(sig).u8(o.flags).u32(1).done() : new W().str(alg).str(sig).done();
        this.send(new W().u8(50).str(user).str("ssh-connection").str("publickey").bool(true).str(alg).str(blob).str(sb).done());
        const p = await this.expect(51, 52);
        return p === null ? "closed" : p[0];
    }
    close() { try { this.c.close(); } catch {} try { this.cl.close(); } catch {} }
}

export function authLine(publicKey, sk) {
    const blob = sk ? new W().str("sk-ssh-ed25519@openssh.com").str(publicKey).str("ssh:").done() : new W().str("ssh-ed25519").str(publicKey).done();
    let s = "";
    for (let i = 0; i < blob.length; i++) s += String.fromCharCode(blob[i]);
    return (sk ? "sk-ssh-ed25519@openssh.com " : "ssh-ed25519 ") + btoa(s);
}
