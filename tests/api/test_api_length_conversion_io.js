// flags: --std
// covers: contract dyna:net.TCPServer.connect dyna:smtp.SmtpClient dyna:smtp.SmtpServer dyna:ssh.SSHAgent.listen dyna:wireguard.WireGuard.create dyna:webrtc.RTCPeerConnection dyna:scrape.Extractor dyna:scrape.Crawl -- one D04 rule (a caller value's length or number read with its conversion result checked) across the I/O modules' option parsers
// D04 st1g: these natives read a caller array's length (or a numeric member) with JS_ToUint32/JS_ToInt64 and
// ignored the -1, so an array whose length (or member) throws left the exception pending while the call went
// on with a zero/stale count: SmtpClient/SmtpServer took an empty mechanism/extension list, send() a stale `to`,
// TCPServer.connect an empty ALPN list, SSHAgent.listen no keys, WireGuard.create no address/peers,
// RTCPeerConnection no ICE urls/local addresses, addIceCandidate mline 0, generateCertificate a 2048-bit default
// (now it rejects with the thrown value, as WebIDL dictionary conversion does), Extractor an empty node list.
// Crawl swallows extractor-side throws by design (a throwing run() emits the page without fields), so a throwing
// links/robots length now drops that list instead of leaving the exception pending under the emitted page.
// st1j (SCRAPE-TOSTRING): Crawl read link and robots entries with JS_ToCString and skipped a NULL without clearing
// the exception, so the next native that consults the exception slot threw the stale sentinel (TOML.stringify below).
// Oracle: every hostile call throws or rejects with exactly the sentinel; the plain call does not.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_api_length_conversion_io");
import { TCPServer } from "dyna:net";
import { SmtpClient, SmtpServer } from "dyna:smtp";
import { SSHAgent } from "dyna:ssh";
import { WireGuard } from "dyna:wireguard";
import { RTCPeerConnection } from "dyna:webrtc";
import { Extractor, Crawl } from "dyna:scrape";
import { TOML } from "dyna:config";
import * as std from "std";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }

const SENT = new Error("length sentinel");
const hostile = (a) => new Proxy(a, { get(t, k) { return k === "length" ? { valueOf() { throw SENT; } } : t[k]; } });
const trapThrows = (a) => new Proxy(a, { get(t, k) { if (k === "length") throw SENT; return t[k]; } });
const plain = (a) => a;
const badNum = { valueOf() { throw SENT; } };
const dir = std.getenv("TMPDIR") || ".";
const H = { host: "127.0.0.1", port: 1, tls: "none" };
const K = await WireGuard.generateKeyPair();
const settle = async (f) => {
    try { const v = await f(); return { v }; } catch (e) { return { e }; }
};
const close = (x) => { try { if (x && typeof x.close === "function") { const r = x.close(); if (r && r.catch) r.catch(() => {}); } } catch (e) {} };

const CASES = [
    ["TCPServer.connect tls.alpn", (w) => TCPServer.connect({ host: "127.0.0.1", port: 1, tls: { alpn: w(["h2"]), rejectUnauthorized: false } }, { error() {}, close() {} })],
    ["SmtpClient mechanisms", (w) => new SmtpClient({ ...H, mechanisms: w(["PLAIN"]) })],
    ["SmtpServer mechanisms", (w) => new SmtpServer({ port: 0, tls: "none", mechanisms: w(["PLAIN"]), onData() { return true; } })],
    ["SmtpServer disable", (w) => new SmtpServer({ port: 0, tls: "none", disable: w(["DSN"]), onData() { return true; } })],
    ["SmtpClient.send to", (w) => { const c = new SmtpClient(H); try { const p = c.send({ from: "a@b.test", to: w(["x@y.test"]), data: "x" }); p.catch(() => {}); } finally { c.close(); } }],
    ["SmtpClient.send dsn.notify", (w) => { const c = new SmtpClient(H); try { const p = c.send({ from: "a@b.test", to: "x@y.test", data: "x", dsn: { notify: w(["SUCCESS"]) } }); p.catch(() => {}); } finally { c.close(); } }],
    ["SSHAgent.listen keys", (w) => SSHAgent.listen({ path: dir + "/st1g-agent-" + n + ".sock", keys: w([]) })],
    ["WireGuard.create address", (w) => WireGuard.create({ privateKey: K.privateKey, address: w(["10.0.0.1/24"]) })],
    ["WireGuard.create peers", (w) => WireGuard.create({ privateKey: K.privateKey, address: ["10.0.0.1/24"], peers: w([]) })],
    ["RTCPeerConnection iceServers urls", (w) => new RTCPeerConnection({ iceServers: [{ urls: w(["stun:127.0.0.1:3478"]) }] })],
    ["RTCPeerConnection iceLocalAddresses", (w) => new RTCPeerConnection({ iceLocalAddresses: w(["127.0.0.1"]) })],
    ["Extractor.run selector result", (w) => new Extractor({ f: { sel: { all() { return w([]); } }, all: true } }).run([])],
    ["Extractor.run node children", (w) => new Extractor({ f: { sel: { all() { return [{ children: w(["t"]) }]; } }, source: true } }).run([])],
];

const SCALARS = [
    ["addIceCandidate sdpMLineIndex", async (bad) => {
        const a = new RTCPeerConnection(), b = new RTCPeerConnection();
        try {
            a.createDataChannel("d");
            await a.setLocalDescription(await a.createOffer());
            await b.setRemoteDescription(a.localDescription);
            return await b.addIceCandidate({ candidate: "candidate:1 1 udp 2122260223 127.0.0.1 9 typ host", sdpMid: null, sdpMLineIndex: bad ? badNum : 0 });
        } finally { a.close(); b.close(); }
    }],
    ["generateCertificate modulusLength", (bad) => RTCPeerConnection.generateCertificate({ name: "RSASSA-PKCS1-v1_5", modulusLength: bad ? badNum : 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" })],
];

for (const [wrap, tag] of [[hostile, "valueOf"], [trapThrows, "get trap"]]) await __kitS.section("a throwing length (" + tag + ") propagates the sentinel", async () => {
    for (const [name, call] of CASES) {
        const r = await settle(() => call(wrap));
        ok(r.e === SENT, name + " (" + tag + "): " + (r.e ? (r.e === SENT ? "sentinel" : "threw " + r.e) : "returned"));
        close(r.v);
    }
});
await __kitS.section("plain arrays still work", async () => {
    for (const [name, call] of CASES) {
        const r = await settle(() => call(plain));
        ok(r.e !== SENT, name + " with a plain array: " + (r.e ? "threw " + r.e : "ok"));
        close(r.v);
    }
});
await __kitS.section("a throwing numeric member rejects with the sentinel", async () => {
    for (const [name, call] of SCALARS) {
        const r = await settle(() => call(true));
        ok(r.e === SENT, name + " (throwing valueOf): " + (r.e ? (r.e === SENT ? "sentinel" : "threw " + r.e) : "resolved"));
        close(r.v);
        const p = await settle(() => call(false));
        ok(p.e !== SENT, name + " plain: " + (p.e ? "threw " + p.e : "ok"));
    }
});

const fetcher = (w) => ({ get(url) { return Promise.resolve({ status: 200, url, body: "", headers: {}, robotsDirectives: w(["noindex"]) }); } });
const ext = (w) => ({ run() { return { ok: true, value: { links: w(["http://a.test/2"]), meta: "nofollow" }, missing: [] }; } });
await __kitS.section("Crawl skips link and robots entries whose toString throws and leaves no exception pending", async () => {
    const bad = { toString() { throw SENT; } };
    for (const [links, robots, tag] of [[[bad, "http://a.test/2"], [], "links"], [[], [bad], "robots"], [[], ["nofollow", bad], "robots tag"]]) {
        const f = { get(url) { return Promise.resolve({ status: 200, url, body: "", headers: {}, robotsDirectives: robots }); } };
        const x = { run() { return { ok: true, value: { links, meta: tag === "robots tag" ? "noarchive" : "" }, missing: [] }; } };
        const c = new Crawl(f, { maxPages: 1, linkField: "links", robotsField: "meta" }).start("http://a.test/1", x);
        const r = await settle(() => c.next());
        let after;
        try { TOML.stringify({ a: Symbol() }); after = "no throw"; } catch (e) { after = e === SENT ? "sentinel leaked" : e instanceof TypeError ? "clean" : "threw " + e; }
        ok(!r.e && r.v && after === "clean", "Crawl " + tag + " with a throwing toString: " + (r.e ? "rejected " + r.e : "page") + ", " + after);
        close(c);
    }
});
await __kitS.section("Crawl drops a list whose length throws and emits the page", async () => {
    for (const [wrap, tag] of [[hostile, "valueOf"], [trapThrows, "get trap"], [plain, "plain"]]) {
        const c = new Crawl(fetcher(wrap), { maxPages: 1, linkField: "links", robotsField: "meta" }).start("http://a.test/1", ext(wrap));
        const r = await settle(() => c.next());
        let after;
        try { TOML.stringify({ a: Symbol() }); after = "no throw"; } catch (e) { after = e === SENT ? "sentinel leaked" : e instanceof TypeError ? "clean" : "threw " + e; }
        ok(!r.e && r.v && r.v.value && r.v.value.value && after === "clean", "Crawl page with " + tag + " links/robots: " + (r.e ? "rejected " + r.e : "page") + ", " + after);
        close(c);
    }
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_api_length_conversion_io: " + n + " checks passed");
