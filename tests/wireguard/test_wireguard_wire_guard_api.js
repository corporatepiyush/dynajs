// flags: --std
// covers: dyna:wireguard.WireGuard
// test_wireguard_wire_guard_api.js -- dyna:wireguard contract without traffic (S13e, 2026-11-27): the class is not constructible,
// create() rejects (never throws) TypeError on every bad option with the offending key or entry in the message, a
// low-order peer key rejects Error after the pool's X25519, the tun open failure carries stage + errno (macOS non-root:
// connect to the utun control socket is EPERM), generateKeyPair runs on the pool, parseConfig reads wg-quick INI
// (case-insensitive keys, comments, repeated AllowedIPs, ignored wg-quick keys) and throws SyntaxError with the line.
import { suite } from "../lib/kit.js";
import { WireGuard } from "dyna:wireguard";
import * as os from "os";

const t = suite("test_wireguard_wire_guard_api");
const ZERO = "A".repeat(43) + "=";
let K1, K2;

t.before(async () => {
    K1 = await WireGuard.generateKeyPair();
    K2 = await WireGuard.generateKeyPair();
});

t.test("not constructible; statics exist", (a) => {
    a.throws(() => new WireGuard(), { type: TypeError, message: "WireGuard: use WireGuard.create(options)" });
    a.eq(typeof WireGuard.create, "function");
    a.eq(typeof WireGuard.parseConfig, "function");
    a.eq(typeof WireGuard.generateKeyPair, "function");
});

t.test("generateKeyPair: base64 of 32 bytes, fresh each call", (a) => {
    a.ok(/^[A-Za-z0-9+/]{43}=$/.test(K1.privateKey) && /^[A-Za-z0-9+/]{43}=$/.test(K1.publicKey), "base64 shape");
    a.ok(K1.privateKey !== K2.privateKey && K1.publicKey !== K2.publicKey, "distinct");
});

t.test("create rejects bad options with TypeError (never throws)", async (a) => {
    const base = { privateKey: "", address: ["10.0.0.1/24"] };
    let threw = false;
    let p;
    try { p = WireGuard.create(); } catch (e) { threw = true; }
    a.ok(!threw && p instanceof Promise, "returns a promise");
    await a.rejects(p, { type: TypeError, message: /expects an options object/ });
    base.privateKey = K1.privateKey;
    await a.rejects(WireGuard.create({ ...base, bogus: 1 }), { type: TypeError, message: /bogus/ });
    await a.rejects(WireGuard.create({ address: ["10.0.0.1"] }), { type: TypeError, message: /privateKey is required/ });
    await a.rejects(WireGuard.create({ ...base, privateKey: "short" }), { type: TypeError, message: /privateKey must be base64 of 32 bytes/ });
    await a.rejects(WireGuard.create({ ...base, privateKey: K1.privateKey.slice(0, 42) + "B=" }), { type: TypeError, message: /base64/ }, "non-canonical trailing bits");
    await a.rejects(WireGuard.create({ privateKey: K1.privateKey }), { type: TypeError, message: /address is required without tun/ });
    await a.rejects(WireGuard.create({ ...base, address: ["10.0.0.300"] }), { type: TypeError, message: /10\.0\.0\.300/ });
    await a.rejects(WireGuard.create({ ...base, address: "10.0.0.1" }), { type: TypeError, message: /address must be an array/ });
    await a.rejects(WireGuard.create({ ...base, listenPort: 70000 }), { type: TypeError, message: /listenPort/ });
    await a.rejects(WireGuard.create({ ...base, mtu: 100 }), { type: TypeError, message: /mtu/ });
    await a.rejects(WireGuard.create({ ...base, peers: {} }), { type: TypeError, message: /peers must be an array/ });
    await a.rejects(WireGuard.create({ ...base, peers: [{ publicKey: K2.publicKey }] }), { type: TypeError, message: /peers\[0\].*allowedIPs/ });
    await a.rejects(WireGuard.create({ ...base, peers: [{ publicKey: K2.publicKey, allowedIPs: [] }] }), { type: TypeError, message: /allowedIPs must not be empty/ });
    await a.rejects(WireGuard.create({ ...base, peers: [{ publicKey: K2.publicKey, allowedIPs: ["10.0.0.2/33"] }] }), { type: TypeError, message: /10\.0\.0\.2\/33/ });
    await a.rejects(WireGuard.create({ ...base, peers: [{ publicKey: K2.publicKey, allowedIPs: ["10.0.0.2/32"], x: 1 }] }), { type: TypeError, message: /\bx\b/ });
    await a.rejects(WireGuard.create({ ...base, peers: [{ publicKey: K2.publicKey, allowedIPs: ["10.0.0.2/32"], endpoint: "[::1]:51820" }] }), { type: TypeError, message: /IPv6 underlay/ });
    await a.rejects(WireGuard.create({ ...base, peers: [{ publicKey: K2.publicKey, allowedIPs: ["10.0.0.2/32"], endpoint: "host.test:51820" }] }), { type: TypeError, message: /endpoint/ });
    await a.rejects(WireGuard.create({ ...base, peers: [{ publicKey: K2.publicKey, allowedIPs: ["10.0.0.2/32"], endpoint: "127.0.0.1:0" }] }), { type: TypeError, message: /endpoint/ });
    await a.rejects(WireGuard.create({ ...base, peers: [{ publicKey: K2.publicKey, allowedIPs: ["10.0.0.2/32"], persistentKeepalive: -1 }] }), { type: TypeError, message: /persistentKeepalive/ });
    const p2 = { publicKey: K2.publicKey, allowedIPs: ["10.0.0.2/32"] };
    await a.rejects(WireGuard.create({ ...base, peers: [p2, p2] }), { type: TypeError, message: /peers\[1\]: duplicate publicKey/ });
    await a.rejects(WireGuard.create({ ...base, tun: "x".repeat(16) }), { type: TypeError, message: /tun name longer than 15/ });
});

t.test("low-order peer key: Error after the pool X25519; the device is closed", async (a) => {
    const e = await a.rejects(WireGuard.create({ privateKey: K1.privateKey, address: ["10.0.0.1"], peers: [{ publicKey: ZERO, allowedIPs: ["10.0.0.2/32"] }] }), { type: Error, message: /low-order public key/ });
    a.ok(!(e instanceof TypeError), "an Error, not a TypeError");
});

t.test("tun open failure carries the stage and errno", async (a) => {
    if (os.platform !== "darwin" || os.getuid?.() === 0) a.skip("macOS non-root only");
    await a.rejects(WireGuard.create({ privateKey: K1.privateKey, tun: "utun99" }),
        { type: TypeError, message: "WireGuard: tun: connect to utun control: Operation not permitted (needs root or CAP_NET_ADMIN)" });
    await a.rejects(WireGuard.create({ privateKey: K1.privateKey, tun: "eth0" }), { type: TypeError, message: /^WireGuard: tun: invalid argument/ });
});

t.test("create, getters, close; methods after close throw", async (a) => {
    const w = await WireGuard.create({ privateKey: K1.privateKey, address: ["10.0.0.1/24", "fd00::1"] });
    a.eq(w.publicKey, K1.publicKey, "public key derived on the pool matches generateKeyPair");
    a.ok(w.listenPort > 0 && w.listenPort < 65536, "ephemeral port");
    a.eq(w.tun, null);
    a.ok(!w.closed);
    const s = w.stats();
    a.eq(s.peers, 0);
    a.eq(s.inlineJobs, 0, "nothing ran inline on the JS thread");
    a.eq(w.removePeer(K2.publicKey), false);
    a.throws(() => w.removePeer("nope"), TypeError);
    w.close();
    w.close();
    a.ok(w.closed);
    a.throws(() => w.stats(), TypeError);
    a.throws(() => w.dial("10.0.0.2", 80), TypeError);
});

t.test("addPeer: duplicate rejects TypeError, low-order rejects Error, remove while pending rejects", async (a) => {
    const w = await WireGuard.create({ privateKey: K1.privateKey, address: ["10.0.0.1"] });
    try {
        await w.addPeer({ publicKey: K2.publicKey, allowedIPs: ["10.0.0.2/32"] });
        a.eq(w.stats().peers, 1);
        await a.rejects(w.addPeer({ publicKey: K2.publicKey, allowedIPs: ["10.0.0.3/32"] }), { type: TypeError, message: /duplicate publicKey/ });
        await a.rejects(w.addPeer({ publicKey: ZERO, allowedIPs: ["10.0.0.4/32"] }), { type: Error, message: /low-order/ });
        a.eq(w.stats().peers, 1, "the failed peer is gone");
        await a.rejects(w.addPeer({ publicKey: "bad", allowedIPs: ["10.0.0.4/32"] }), TypeError);
        const k3 = await WireGuard.generateKeyPair();
        const p = w.addPeer({ publicKey: k3.publicKey, allowedIPs: ["10.0.0.5/32"] });
        a.eq(w.removePeer(k3.publicKey), true);
        await a.rejects(p, { type: Error, message: /removed before it was ready/ });
        a.eq(w.removePeer(K2.publicKey), true);
        a.eq(w.stats().peers, 0);
        const k4 = await WireGuard.generateKeyPair();
        const p4 = w.addPeer({ publicKey: k4.publicKey, allowedIPs: ["10.0.0.6/32"] });
        w.close();
        await a.rejects(p4, { type: Error, message: "WireGuard: closed" });
    } finally {
        w.close();
    }
});

const CONF = `
# a wg-quick file
[Interface]
PrivateKey = ${"yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk="}
ListenPort = 51820
Address = 10.200.100.8/24, fd42::8/64
Address=10.200.101.8
DNS = 10.200.100.1
PostUp = iptables -A FORWARD -i %i -j ACCEPT
MTU = 1380

[peer]
publickey = xTIBA5rboUvnH4htodjb6e697QjLERt1NAB4mZqp8Dg=
PresharedKey = /UwcSPg38hW/D9Y3tcS1FOV0K1wuURMbS0sesJEP5ak=
AllowedIPs = 10.200.100.0/24 # tunnel subnet
allowedips = 0.0.0.0/0
Endpoint = 192.95.5.69:51820
PersistentKeepalive = 25

[Peer]
PublicKey = TrMvSoP4jYQlY6RIzBgbssQqY3vxI2Pi+y71lOWWXX0=
AllowedIPs = 10.10.10.230/32
PersistentKeepalive = off
`;

t.test("parseConfig: wg-quick vectors", (a) => {
    const c = WireGuard.parseConfig(CONF);
    a.eq(c.privateKey, "yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=");
    a.eq(c.listenPort, 51820);
    a.eq(c.mtu, 1380);
    a.deep(c.address, ["10.200.100.8/24", "fd42::8/64", "10.200.101.8"]);
    a.eq(c.peers.length, 2);
    a.deep(c.peers[0], { allowedIPs: ["10.200.100.0/24", "0.0.0.0/0"], publicKey: "xTIBA5rboUvnH4htodjb6e697QjLERt1NAB4mZqp8Dg=", presharedKey: "/UwcSPg38hW/D9Y3tcS1FOV0K1wuURMbS0sesJEP5ak=", endpoint: "192.95.5.69:51820", persistentKeepalive: 25 });
    a.deep(c.peers[1], { allowedIPs: ["10.10.10.230/32"], publicKey: "TrMvSoP4jYQlY6RIzBgbssQqY3vxI2Pi+y71lOWWXX0=", persistentKeepalive: 0 });
    a.ok(!("dns" in c) && !("DNS" in c), "wg-quick-only keys are dropped");
    a.deep(WireGuard.parseConfig(""), { address: [], peers: [] });
    a.deep(WireGuard.parseConfig("[Interface]\r\nPrivateKey = k\r\n[Peer]\r\n").peers, [{ allowedIPs: [] }], "CRLF, an empty [Peer]");
});

t.test("parseConfig: SyntaxError with the line", (a) => {
    const bad = [
        ["[Interface]\nFoo = 1", "line 2: unknown key"],
        ["PrivateKey = x", "line 1: key outside a section"],
        ["[Interface]\nListenPort = 1\nListenPort = 2", "line 3: duplicate key"],
        ["[Interface]\nListenPort = 99999", "line 2: bad number"],
        ["[Interface]\nListenPort = -1", "line 2: bad number"],
        ["[Interface]\n[Interface]", "line 2: duplicate [Interface]"],
        ["[Wat]", "line 1: unknown section"],
        ["[Peer", "line 1: unterminated section header"],
        ["[Peer]\nPublicKey", "line 2: expected key = value"],
        ["[Peer]\nPublicKey = ", "line 2: empty value"],
        ["[Peer]\nPrivateKey = x", "line 2: unknown key"],
        ["[Interface]\n" + "#".repeat(5000), "line 2: line longer than 4096 bytes"],
        ["[Interface]\nPrivateKey = a\u0000b", "line 0: NUL byte in the text"],
    ];
    for (const [txt, msg] of bad)
        a.throws(() => WireGuard.parseConfig(txt), { type: SyntaxError, message: "WireGuard.parseConfig: " + msg }, JSON.stringify(txt.slice(0, 30)));
    a.throws(() => WireGuard.parseConfig(5), TypeError);
});

t.test("parseConfig output feeds create", async (a) => {
    const txt = `[Interface]\nPrivateKey = ${K1.privateKey}\nAddress = 10.9.9.1/24\n[Peer]\nPublicKey = ${K2.publicKey}\nAllowedIPs = 10.9.9.2/32\nEndpoint = 127.0.0.1:9\n`;
    const w = await WireGuard.create(WireGuard.parseConfig(txt));
    a.eq(w.publicKey, K1.publicKey);
    a.eq(w.stats().peers, 1);
    w.close();
});

await t.run();
