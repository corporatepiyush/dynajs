// flags: --std
// End-to-end encrypted messenger — X3DH key agreement, the Double Ratchet, sender-key groups, encrypted attachments
// and safety numbers, over a relay server that only ever sees ciphertext.
//
// WHAT IT IS
//   The Signal protocol's moving parts, implemented on the engine's primitives:
//
//   identity     each user has an Ed25519 signing identity and an X25519 identity key; a signed prekey and a stack
//                of one-time prekeys are published to the server as a "prekey bundle"
//   X3DH         a sender fetches the bundle, checks the prekey signature, and derives the shared secret from four
//                Diffie-Hellman outputs; each one-time prekey is handed out exactly once
//   ratchet      every message advances a symmetric chain (HMAC), every reply turn advances a Diffie-Hellman
//                ratchet (HKDF), so a stolen key opens neither past messages nor (after one round trip) future ones;
//                out-of-order and lost messages are handled with bounded skipped-key storage
//   transport    a WebSocket relay with per-user mailboxes for offline delivery; it routes opaque envelopes and
//                cannot read, alter (AEAD with the header as associated data) or replay them
//   groups       sender keys: each member distributes its own chain key over the pairwise sessions, then a group
//                message is encrypted once, signed, and fanned out by the server
//   attachments  encrypted with a random key, uploaded as an opaque blob over HTTP, referenced by digest
//   safety       60-digit safety numbers from both identity keys; a changed identity changes the number
//   replay       the relay remembers envelope ids in a CuckooFilter (about a byte per id, with deletion when an id
//                ages out of the RingBuffer window) and drops a re-sent envelope before it reaches a mailbox
//
// MODULES  dyna:crypto (X25519, Ed25519, HKDF, HMAC, ChaCha20Poly1305/AESGCM) · dyna:http (App, WsClient)
//          · dyna:serialize (CBOR) · dyna:encoding (Base64) · dyna:hash (SHA512) · dyna:structures (CuckooFilter,
//          RingBuffer) · dyna:bytes (concat) · dyna:random
//
// RUN      dynajs --std examples/security/e2e-messenger.js     three users exchange direct, offline, group and attachment messages

import * as crypto from "dyna:crypto";
import { App, WsClient } from "dyna:http";
import { CBOREncode, CBORDecode } from "dyna:serialize";
import { Base64Encode, Base64Decode, HexEncode } from "dyna:encoding";
import { SHA512, SHA256Hex } from "dyna:hash";
import { CuckooFilter, RingBuffer } from "dyna:structures";
import { concat } from "dyna:bytes";
import { Random } from "dyna:random";
import * as std from "std";

if (typeof crypto.X25519Generate !== "function" || typeof crypto.Ed25519Generate !== "function") {
    print("e2e-messenger: this build has no X25519/Ed25519; nothing to demonstrate");
    std.exit(0);
}
const AEAD = typeof crypto.ChaCha20Poly1305 === "function" ? crypto.ChaCha20Poly1305 : crypto.AESGCM;
const enc = new TextEncoder(),
    dec = new TextDecoder();
const cat = (...parts) => concat(parts);
const eq = (a, b) => a.length === b.length && crypto.TimingSafeEqual(a, b);
const hex = (u8) => HexEncode(u8);

// =============================================================================
// Primitives
// =============================================================================
const DH = (priv, pub) => crypto.X25519Derive(priv, pub);
const kdfRK = (rk, dhOut) => {
    const o = crypto.HKDF({ hash: "sha256", key: dhOut, salt: rk, info: "e2e-ratchet", length: 64 });
    return [o.slice(0, 32), o.slice(32)];
};
const kdfCK = (ck) => [crypto.HMAC("sha256", ck, new Uint8Array([2])), crypto.HMAC("sha256", ck, new Uint8Array([1]))]; // [next chain, message key]
function aeadSeal(mk, plaintext, ad) {
    const o = crypto.HKDF({ hash: "sha256", key: mk, salt: new Uint8Array(32), info: "e2e-msg", length: 44 });
    const a = new AEAD(o.slice(0, 32));
    const out = a.seal(o.slice(32), plaintext, ad);
    a.close?.();
    return out;
}
function aeadOpen(mk, sealed, ad) {
    const o = crypto.HKDF({ hash: "sha256", key: mk, salt: new Uint8Array(32), info: "e2e-msg", length: 44 });
    const a = new AEAD(o.slice(0, 32));
    try {
        return a.open(o.slice(32), sealed, ad);
    } finally {
        a.close?.();
    }
}

// =============================================================================
// Double Ratchet session
// =============================================================================
const MAX_SKIP = 50;
class Session {
    static initiator(sk, theirRatchetPub, ad) {
        const s = new Session(ad);
        s.DHs = crypto.X25519Generate();
        s.DHr = theirRatchetPub;
        [s.RK, s.CKs] = kdfRK(sk, DH(s.DHs.privateKey, s.DHr));
        return s;
    }
    static responder(sk, ourRatchet, ad) {
        const s = new Session(ad);
        s.DHs = ourRatchet;
        s.RK = sk;
        return s;
    }
    constructor(ad) {
        this.ad = ad;
        this.CKs = null;
        this.CKr = null;
        this.Ns = 0;
        this.Nr = 0;
        this.PN = 0;
        this.skipped = new Map();
    }
    encrypt(plaintext) {
        let mk;
        [this.CKs, mk] = kdfCK(this.CKs);
        const header = CBOREncode({ dh: this.DHs.publicKey, pn: this.PN, n: this.Ns });
        this.Ns++;
        return { header, body: aeadSeal(mk, plaintext, cat(this.ad, header)) };
    }
    decrypt({ header, body }) {
        const h = CBORDecode(header),
            skipKey = `${hex(h.dh)}:${h.n}`;
        if (this.skipped.has(skipKey)) {
            const mk = this.skipped.get(skipKey);
            this.skipped.delete(skipKey);
            return aeadOpen(mk, body, cat(this.ad, header));
        }
        const snapshot = { ...this, skipped: new Map(this.skipped) }; // roll back if authentication fails
        try {
            if (!this.DHr || !eq(h.dh, this.DHr)) {
                this.skip(h.pn);
                this.PN = this.Ns;
                this.Ns = 0;
                this.Nr = 0;
                this.DHr = h.dh;
                [this.RK, this.CKr] = kdfRK(this.RK, DH(this.DHs.privateKey, this.DHr));
                this.DHs = crypto.X25519Generate();
                [this.RK, this.CKs] = kdfRK(this.RK, DH(this.DHs.privateKey, this.DHr));
            }
            this.skip(h.n);
            let mk;
            [this.CKr, mk] = kdfCK(this.CKr);
            this.Nr++;
            return aeadOpen(mk, body, cat(this.ad, header));
        } catch (e) {
            Object.assign(this, snapshot);
            throw e;
        }
    }
    skip(until) {
        if (!this.CKr) return;
        if (until - this.Nr > MAX_SKIP) throw new Error("too many skipped messages");
        while (this.Nr < until) {
            let mk;
            [this.CKr, mk] = kdfCK(this.CKr);
            this.skipped.set(`${hex(this.DHr)}:${this.Nr}`, mk);
            this.Nr++;
        }
    }
}

// =============================================================================
// Relay server: prekey directory, mailboxes, blobs, group fan-out
// =============================================================================
const REPLAY_WINDOW = 50_000;
const server = {
    bundles: new Map(),
    mailboxes: new Map(),
    sockets: new Map(),
    blobs: new Map(),
    groups: new Map(),
    seen: new CuckooFilter(REPLAY_WINDOW * 2),
    window: new RingBuffer(REPLAY_WINDOW),
    replays: 0,
    log: [],
};
const relay = new App({ port: 0 });
relay.post("/keys/:user", (req) => {
    server.bundles.set(req.params.user, CBORDecode(Base64Decode(req.body)));
    return { status: 204, body: "" };
});
relay.get("/keys/:user", (req) => {
    const b = server.bundles.get(req.params.user);
    if (!b) return { status: 404, body: "" };
    const otk = b.oneTime.shift() || null; // each one-time prekey is served once
    return {
        body: Base64Encode(CBOREncode({ ik: b.ik, sik: b.sik, spk: b.spk, spkSig: b.spkSig, otk })),
        contentType: "text/plain",
    };
});
relay.post("/blobs", (req) => {
    const id = SHA256Hex(req.body);
    server.blobs.set(id, req.body);
    return { id };
});
relay.get("/blobs/:id", (req) =>
    server.blobs.has(req.params.id)
        ? { body: server.blobs.get(req.params.id), contentType: "text/plain" }
        : { status: 404, body: "" },
);
function deliver(to, envelope) {
    server.log.push(envelope);
    const ws = server.sockets.get(to);
    if (ws) ws.send(envelope);
    else {
        if (!server.mailboxes.has(to)) server.mailboxes.set(to, []);
        server.mailboxes.get(to).push(envelope);
    }
}
relay.ws("/socket", {
    message: (ws, data) => {
        const m = JSON.parse(data);
        if (m.type === "login") {
            server.sockets.set(m.user, ws);
            ws.user = m.user;
            for (const e of server.mailboxes.get(m.user) || []) ws.send(e);
            server.mailboxes.delete(m.user);
            return;
        }
        if (!ws.user) return;
        if (server.seen.mayContain(m.id)) {
            server.replays++;
            return;
        } // relay-level replay suppression
        if (server.window.length === REPLAY_WINDOW) server.seen.delete(server.window.get(0)); // the oldest id ages out
        server.window.push(m.id);
        server.seen.add(m.id);
        if (m.type === "direct") deliver(m.to, data);
        if (m.type === "group")
            for (const member of server.groups.get(m.group) || []) if (member !== ws.user) deliver(member, data);
    },
    close: (ws) => {
        if (ws.user && server.sockets.get(ws.user) === ws) server.sockets.delete(ws.user);
    },
});
relay.start();
const base = `http://127.0.0.1:${relay.port}`;

// =============================================================================
// Client
// =============================================================================
class Client {
    constructor(name) {
        this.name = name;
        this.sign = crypto.Ed25519Generate();
        this.ik = crypto.X25519Generate();
        this.spk = crypto.X25519Generate();
        this.oneTime = Array.from({ length: 5 }, () => crypto.X25519Generate());
        this.sessions = new Map();
        this.inbox = [];
        this.senderKeys = new Map();
        this.myGroupChains = new Map();
        this.waiters = [];
    }
    async publish() {
        const bundle = {
            ik: this.ik.publicKey,
            sik: this.sign.publicKey,
            spk: this.spk.publicKey,
            spkSig: crypto.Ed25519Sign(this.sign.privateKey, this.spk.publicKey),
            oneTime: this.oneTime.map((k) => k.publicKey),
        };
        await (
            await fetch(`${base}/keys/${this.name}`, { method: "POST", body: Base64Encode(CBOREncode(bundle)) })
        ).text();
    }
    connect() {
        return new Promise((resolve, reject) => {
            this.ws = new WsClient(`ws://127.0.0.1:${relay.port}/socket`, {
                open: (ws) => {
                    ws.send(JSON.stringify({ type: "login", user: this.name }));
                    setTimeout(resolve, 20);
                },
                message: (ws, data) => this.receive(JSON.parse(data)),
                close: () => {},
            });
            setTimeout(() => reject(new Error("connect timeout")), 3000);
        });
    }
    disconnect() {
        this.ws.close();
    }
    async startSession(peer) {
        const b = CBORDecode(Base64Decode(await (await fetch(`${base}/keys/${peer}`)).text()));
        if (!crypto.Ed25519Verify(b.sik, b.spk, b.spkSig)) throw new Error(`${peer}'s signed prekey does not verify`);
        const ek = crypto.X25519Generate();
        const parts = [DH(this.ik.privateKey, b.spk), DH(ek.privateKey, b.ik), DH(ek.privateKey, b.spk)];
        if (b.otk) parts.push(DH(ek.privateKey, b.otk));
        const sk = crypto.HKDF({
            hash: "sha256",
            key: cat(new Uint8Array(32).fill(0xff), ...parts),
            salt: new Uint8Array(32),
            info: "e2e-x3dh",
            length: 32,
        });
        const ad = cat(this.ik.publicKey, b.ik);
        const s = Session.initiator(sk, b.spk, ad);
        s.init = { ik: this.ik.publicKey, sik: this.sign.publicKey, ek: ek.publicKey, otk: b.otk };
        s.peerIdentity = b.ik;
        this.sessions.set(peer, s);
        return s;
    }
    acceptSession(peer, init) {
        const otk = init.otk ? this.oneTime.find((k) => eq(k.publicKey, init.otk)) : null;
        if (init.otk && !otk) throw new Error("one-time prekey already used");
        const parts = [
            DH(this.spk.privateKey, init.ik),
            DH(this.ik.privateKey, init.ek),
            DH(this.spk.privateKey, init.ek),
        ];
        if (otk) {
            parts.push(DH(otk.privateKey, init.ek));
            this.oneTime = this.oneTime.filter((k) => k !== otk);
        }
        const sk = crypto.HKDF({
            hash: "sha256",
            key: cat(new Uint8Array(32).fill(0xff), ...parts),
            salt: new Uint8Array(32),
            info: "e2e-x3dh",
            length: 32,
        });
        const s = Session.responder(sk, this.spk, cat(init.ik, this.ik.publicKey));
        s.peerIdentity = init.ik;
        s.peerSigning = init.sik;
        this.sessions.set(peer, s);
        return s;
    }
    async send(peer, payload) {
        const s = this.sessions.get(peer) || (await this.startSession(peer));
        const m = s.encrypt(CBOREncode(payload));
        const env = {
            type: "direct",
            id: crypto.RandomBytes(12).reduce((a, b) => a + b.toString(16).padStart(2, "0"), ""),
            from: this.name,
            to: peer,
            header: Base64Encode(m.header),
            body: Base64Encode(m.body),
            init:
                s.Nr === 0 && s.init
                    ? {
                          ...s.init,
                          ik: Base64Encode(s.init.ik),
                          sik: Base64Encode(s.init.sik),
                          ek: Base64Encode(s.init.ek),
                          otk: s.init.otk && Base64Encode(s.init.otk),
                      }
                    : undefined,
        };
        this.lastEnvelope = env;
        this.ws.send(JSON.stringify(env));
        return env;
    }
    receive(env) {
        try {
            if (env.type === "direct") {
                let s = this.sessions.get(env.from);
                if (!s && env.init)
                    s = this.acceptSession(env.from, {
                        ik: Base64Decode(env.init.ik),
                        sik: Base64Decode(env.init.sik),
                        ek: Base64Decode(env.init.ek),
                        otk: env.init.otk && Base64Decode(env.init.otk),
                    });
                const payload = CBORDecode(
                    s.decrypt({ header: Base64Decode(env.header), body: Base64Decode(env.body) }),
                );
                if (payload.kind === "sender-key")
                    this.senderKeys.set(`${payload.group}:${env.from}`, { ck: payload.ck, n: 0, pub: payload.pub });
                else this.inbox.push({ from: env.from, ...payload });
            } else if (env.type === "group") {
                const sk = this.senderKeys.get(`${env.group}:${env.from}`);
                const sealed = Base64Decode(env.body),
                    sig = Base64Decode(env.sig);
                if (!crypto.Ed25519Verify(sk.pub, cat(enc.encode(`${env.group}:${env.n}`), sealed), sig))
                    throw new Error("bad group signature");
                while (sk.n < env.n) {
                    [sk.ck] = kdfCK(sk.ck);
                    sk.n++;
                }
                let mk;
                [sk.ck, mk] = kdfCK(sk.ck);
                sk.n++;
                this.inbox.push({
                    from: env.from,
                    group: env.group,
                    ...CBORDecode(aeadOpen(mk, sealed, enc.encode(env.group))),
                });
            }
        } catch (e) {
            this.inbox.push({ from: env.from, error: e.message });
        }
        for (const w of this.waiters.splice(0)) w();
    }
    async waitFor(pred, ms = 3000) {
        const deadline = Date.now() + ms;
        while (!pred()) {
            if (Date.now() > deadline) throw new Error(`${this.name}: timed out waiting`);
            await new Promise((r) => {
                this.waiters.push(r);
                setTimeout(r, 50);
            });
        }
    }
    async createGroup(group, members) {
        server.groups.set(group, [this.name, ...members]);
        const chain = { ck: crypto.RandomBytes(32), n: 0, sign: crypto.Ed25519Generate() };
        this.myGroupChains.set(group, chain);
        for (const m of members)
            await this.send(m, { kind: "sender-key", group, ck: chain.ck, pub: chain.sign.publicKey });
    }
    async joinGroupKeys(group, members) {
        const chain = { ck: crypto.RandomBytes(32), n: 0, sign: crypto.Ed25519Generate() };
        this.myGroupChains.set(group, chain);
        for (const m of members)
            await this.send(m, { kind: "sender-key", group, ck: chain.ck, pub: chain.sign.publicKey });
    }
    sendGroup(group, payload) {
        const c = this.myGroupChains.get(group);
        let mk;
        [c.ck, mk] = kdfCK(c.ck);
        const sealed = aeadSeal(mk, CBOREncode(payload), enc.encode(group));
        const env = {
            type: "group",
            id: HexEncode(crypto.RandomBytes(12)),
            from: this.name,
            group,
            n: c.n,
            body: Base64Encode(sealed),
            sig: Base64Encode(crypto.Ed25519Sign(c.sign.privateKey, cat(enc.encode(`${group}:${c.n}`), sealed))),
        };
        c.n++;
        this.ws.send(JSON.stringify(env));
    }
    async sendAttachment(peer, bytes, name) {
        const key = crypto.RandomBytes(32),
            sealed = aeadSeal(key, bytes, enc.encode(name));
        const { id } = await (await fetch(`${base}/blobs`, { method: "POST", body: Base64Encode(sealed) })).json();
        return this.send(peer, {
            kind: "attachment",
            name,
            blob: id,
            key,
            size: bytes.length,
            digest: SHA256Hex(bytes),
        });
    }
    async fetchAttachment(msg) {
        const sealed = Base64Decode(await (await fetch(`${base}/blobs/${msg.blob}`)).text());
        const bytes = aeadOpen(msg.key, sealed, enc.encode(msg.name));
        if (SHA256Hex(bytes) !== msg.digest) throw new Error("attachment digest mismatch");
        return bytes;
    }
    safetyNumber(peer) {
        const fp = (ik, id) => {
            let h = cat(enc.encode(id), ik);
            for (let i = 0; i < 5200; i++) h = SHA512(cat(h, ik));
            return h;
        };
        const mine = fp(this.ik.publicKey, this.name),
            theirs = fp(this.sessions.get(peer).peerIdentity, peer);
        const chunks = (h) =>
            Array.from({ length: 6 }, (_, i) =>
                String(
                    ((h[i * 5] * 2 ** 32 +
                        ((h[i * 5 + 1] << 24) | (h[i * 5 + 2] << 16) | (h[i * 5 + 3] << 8) | h[i * 5 + 4])) >>>
                        0) %
                        100000,
                ).padStart(5, "0"),
            );
        const [a, b] = this.name < peer ? [mine, theirs] : [theirs, mine];
        return [...chunks(a), ...chunks(b)].join(" ");
    }
}

// =============================================================================
// Conversation
// =============================================================================
const results = {};
const alice = new Client("alice"),
    bob = new Client("bob"),
    carol = new Client("carol");
for (const c of [alice, bob, carol]) await c.publish();
await alice.connect();
await bob.connect();

await alice.send("bob", { text: "hi bob, this is alice" });
await bob.waitFor(() => bob.inbox.length === 1);
results["X3DH + first ratchet message decrypts"] = bob.inbox[0].text === "hi bob, this is alice";
results["the one-time prekey was consumed from the directory"] = server.bundles.get("bob").oneTime.length === 4;
await bob.send("alice", { text: "hey alice" });
for (let i = 0; i < 5; i++) await alice.send("bob", { text: `message ${i}` });
await bob.send("alice", { text: "got them" });
await alice.waitFor(() => alice.inbox.length === 2);
await bob.waitFor(() => bob.inbox.length === 6);
results["a run of messages in one chain"] =
    bob.inbox
        .slice(1)
        .map((m) => m.text)
        .join("|") === "message 0|message 1|message 2|message 3|message 4";
results[`safety numbers agree: ${alice.safetyNumber("bob").slice(0, 23)}…`] =
    alice.safetyNumber("bob") === bob.safetyNumber("alice") && alice.safetyNumber("bob").split(" ").length === 12;

// Out of order: encrypt three, deliver 3, 1, 2 directly to bob's receive().
const sA = alice.sessions.get("bob");
const pkts = [1, 2, 3].map((i) => sA.encrypt(CBOREncode({ text: `ooo ${i}` })));
const wrapEnv = (m) => ({ type: "direct", from: "alice", header: Base64Encode(m.header), body: Base64Encode(m.body) });
for (const k of [2, 0, 1]) bob.receive(wrapEnv(pkts[k]));
results["out-of-order delivery decrypts via skipped keys"] =
    bob.inbox
        .slice(-3)
        .map((m) => m.text)
        .join("|") === "ooo 3|ooo 1|ooo 2" && bob.sessions.get("alice").skipped.size === 0;

// Tampering and replay.
const t = sA.encrypt(CBOREncode({ text: "transfer 10 EUR" }));
const body = Base64Decode(Base64Encode(t.body));
body[3] ^= 1;
bob.receive({ type: "direct", from: "alice", header: Base64Encode(t.header), body: Base64Encode(body) });
results["a modified ciphertext fails authentication"] = bob.inbox[bob.inbox.length - 1].error;
bob.receive(wrapEnv(t));
bob.receive(wrapEnv(t));
results["a replayed message does not decrypt twice"] =
    bob.inbox[bob.inbox.length - 2].text === "transfer 10 EUR" && bob.inbox[bob.inbox.length - 1].error;

// A replay through the relay itself is dropped before it reaches a mailbox.
const before_ = server.log.length;
alice.ws.send(JSON.stringify(alice.lastEnvelope));
await new Promise((r) => setTimeout(r, 50));
results["the relay drops a re-sent envelope by id (CuckooFilter)"] =
    server.replays === 1 && server.log.length === before_;

// The server never saw plaintext.
const transcript = JSON.stringify(server.log);
results["the relay's log contains no plaintext"] = !/alice, this is|message 3|got them|transfer/.test(transcript);

// Offline delivery: carol is offline when bob writes.
await bob.send("carol", { text: "welcome carol (sent while you were offline)" });
for (let i = 0; i < 100 && !server.mailboxes.get("carol"); i++) await new Promise((r) => setTimeout(r, 10));
results["the relay holds the envelope for an offline user"] = server.mailboxes.get("carol")?.length === 1;
await carol.connect();
await carol.waitFor(() => carol.inbox.length === 1);
results["the mailbox drains on login and the session is established from it"] =
    carol.inbox[0].text.startsWith("welcome carol");

// Forward secrecy: keys taken from bob's session NOW cannot open earlier messages.
const stolen = { ...bob.sessions.get("alice") };
const early = alice.lastEnvelope;
let fs = "opened";
try {
    const thief = Object.assign(Object.create(Session.prototype), stolen, { skipped: new Map() });
    thief.decrypt({ header: Base64Decode(early.header), body: Base64Decode(early.body) });
} catch {
    fs = "refused";
}
results["a compromised current state does not decrypt already-delivered messages"] = fs === "refused";

// Groups with sender keys.
await alice.createGroup("project-x", ["bob", "carol"]);
await bob.waitFor(() => bob.senderKeys.has("project-x:alice"));
await carol.waitFor(() => carol.senderKeys.has("project-x:alice"));
await bob.joinGroupKeys("project-x", ["alice", "carol"]);
await alice.waitFor(() => alice.senderKeys.has("project-x:bob"));
await carol.waitFor(() => carol.senderKeys.has("project-x:bob"));
alice.sendGroup("project-x", { text: "kickoff at 10" });
bob.sendGroup("project-x", { text: "I'll bring the slides" });
alice.sendGroup("project-x", { text: "great" });
await carol.waitFor(() => carol.inbox.filter((m) => m.group).length === 3);
results["group messages decrypt for every member"] =
    carol.inbox
        .filter((m) => m.group)
        .map((m) => `${m.from}:${m.text}`)
        .join("|")
        .includes("alice:kickoff at 10") && carol.inbox.filter((m) => m.group && m.error).length === 0;

// Attachments.
const photo = new Random(9).bytes(200_000);
await alice.sendAttachment("bob", photo, "whiteboard.png");
await bob.waitFor(() => bob.inbox.some((m) => m.kind === "attachment"));
const got = await bob.fetchAttachment(bob.inbox.find((m) => m.kind === "attachment"));
results["a 200 KB attachment round-trips through an opaque blob"] =
    SHA256Hex(got) === SHA256Hex(photo) &&
    !server.blobs
        .values()
        .next()
        .value.includes(Base64Encode(photo.slice(0, 30)).slice(0, 36));

// Identity change is visible.
const before = alice.safetyNumber("bob");
alice.sessions.get("bob").peerIdentity = crypto.X25519Generate().publicKey;
results["a different identity key changes the safety number"] = alice.safetyNumber("bob") !== before;

print("e2e-messenger");
print(`  cipher     X3DH (4 DH) + Double Ratchet, ${AEAD.name}, Ed25519-signed prekeys`);
print(
    `  traffic    ${server.log.length} envelopes relayed, ${server.blobs.size} blob(s); alice->bob chain at n=${alice.sessions.get("bob").Ns}, bob's DH ratchet turned ${alice.sessions.get("bob").PN ? "yes" : "no"}`,
);
print(`  safety     alice/bob ${before}`);
print(`  replays    ${server.replays} dropped at the relay; ${server.window.length} ids in the replay window`);
print(
    `  inboxes    alice ${alice.inbox.length}, bob ${bob.inbox.length} (${bob.inbox.filter((m) => m.error).length} rejected), carol ${carol.inbox.length}`,
);
for (const c of [alice, bob, carol]) c.disconnect();
relay.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
