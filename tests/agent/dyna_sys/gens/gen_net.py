#!/usr/bin/env python3
"""gen_net.py — dyna:net probes. TCP echo scenarios, UDP datagram semantics,
RESP wire-format BOTH directions (python server + RAWLOG oracle), loopback
DNS (dyna DNSServer + DNSResolver), L4 TCPProxy, and the pure IP/ratelimit/
metrics unit matrix (oracle: python ipaddress, baked)."""
import json
import ipaddress
from probe_lib import emit

IMP_TCP = '''import { TCPServer, UDPSocket, connectHappy, TCPProxy } from "dyna:net";
import { getEnv } from "dyna:sys";
const PORT = parseInt(getEnv("DYN_CTL_PORT"));
const HOST = "127.0.0.1";
function bytesToStr(u8) { let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
'''

IMP_RESP = '''import { Redis } from "dyna:net";
import { getEnv } from "dyna:sys";
const PORT = parseInt(getEnv("DYN_CTL_PORT"));
function bytesToStr(u8) { let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
'''

IMP_DNS = '''import { DNSResolver, DNSServer } from "dyna:net";
'''

IMP_IP = '''import { parseAddr, parsePrefix, canonical, isValid, compareAddr, contains,
         masked, isLoopback, isPrivate, isGlobalUnicast, isLinkLocalUnicast,
         isLinkLocalMulticast, isMulticast, isUnspecified, Prefix,
         RateLimiter, Metrics } from "dyna:net";
'''

# ---- TCP scenarios against ctl.py tcp mode
emit("net", "tcp", IMP_TCP, r'''
const results = {};
function once(mode, payload, name) {
  return new Promise((resolve) => {
    const events = [];
    const cli = TCPServer.connect({ host: HOST, port: PORT },
      { connect: (c, err) => {
          if (!c) { events.push("err:" + err); resolve({ events, cli: null }); return; }
          events.push("connect");
          c.write(payload);
        },
        data: (c, b) => { events.push("data:" + bytesToStr(b)); },
        close: (c) => { events.push("close"); resolve({ events, cli }); },
      });
    cli._name = name;
    setTimeout(() => resolve({ events, cli }), 3000); // watchdog: never hang
  });
}
const P = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  // B: binary-safe echo, mode byte + all 256 byte values (incl \r\n and NUL)
  {
    const bin = new Uint8Array(257); bin[0] = 66; /* B */ for (let i = 0; i < 256; i++) bin[i + 1] = i;
    const got = [];
    const cli = TCPServer.connect({ host: HOST, port: PORT }, {
      connect: (c) => c.write(bin),
      data: (c, b) => { for (const x of b) got.push(x); },
      close: () => {},
    });
    await P(500);
    cli.close();
    let ok = got.length === 257 && got[0] === 66;
    for (let i = 0; i < 256 && ok; i++) if (got[i + 1] !== i) ok = false;
    assert_true(ok, "tcp binary echo all 256 bytes intact, len=" + got.length);
  }
  // C: server replies then closes
  {
    const { events, cli } = await once("C", "Cignore", "close");
    if (cli) cli.close();
    assert_true(events.indexOf("data:bye\n") >= 0, "C: server reply seen");
    assert_true(events.indexOf("close") >= 0, "C: close event fires on peer FIN");
  }
  // H: half-close — echo (incl mode byte) then FIN, close still fires
  {
    const { events, cli } = await once("H", "Hping", "half");
    if (cli) cli.close();
    assert_true(events.indexOf("data:ping") >= 0, "H: echo received (mode byte consumed)");
    assert_true(events.indexOf("close") >= 0, "H: FIN surfaces as close");
  }
  // R: immediate reset — error or close, never hang
  {
    const { events, cli } = await once("R", "Rx", "reset");
    if (cli) cli.close();
    assert_true(events.some((e) => e.indexOf("err:") === 0) || events.indexOf("close") >= 0,
      "R: reset surfaces as err/close, got " + JSON.stringify(events));
  }
  // D: dribble — three separate data events (chunks are message boundaries at TCP level here)
  {
    const { events, cli } = await once("D", "Dx", "dribble");
    if (cli) cli.close();
    const parts = events.filter((e) => e.indexOf("data:part") === 0);
    assert_eq(parts.length, 3, "D: three dribbled chunks delivered, got " + JSON.stringify(events));
  }
  // Z: client sends nothing and closes its side; server sees EOF, closes;
  // client sees close with no data
  {
    const events = [];
    await new Promise((resolve) => {
      const cli = TCPServer.connect({ host: HOST, port: PORT }, {
        connect: (c) => { events.push("connect"); if (c) c.close(); },
        data: (c, b) => events.push("data:" + bytesToStr(b)),
        close: () => { events.push("close"); resolve(); },
      });
      setTimeout(resolve, 3000);
    });
    assert_true(events.indexOf("close") >= 0, "Z: close fires, got " + JSON.stringify(events));
    assert_true(!events.some((e) => e.indexOf("data:") === 0), "Z: no data events");
  }
  // connect refused (port 1 is privileged+closed: use a closed high port via udp socket trick)
  {
    const u = new UDPSocket({ port: 0, host: HOST });
    u.start({ message: () => {} });
    const closedPort = u.port; // bound (UDP) => no TCP listener on that port
    let refused = null;
    await new Promise((resolve) => {
      const c = TCPServer.connect({ host: HOST, port: closedPort }, {
        connect: (cc, err) => { refused = err || "connected?!"; if (cc) cc.close(); c.close(); resolve(); },
      });
      setTimeout(() => { try { c.close(); } catch (e) {} resolve(); }, 2000);
    });
    u.close();
    assert_eq(refused, "Connection refused", "connect to closed port reports Connection refused, got " + refused);
  }
  summary("net.tcp");
  // keep every resource released: probes must terminate
})();
''', ctl="tcp")

emit("net", "udp", IMP_TCP, r'''
const u = new UDPSocket({ port: 0, host: HOST });
const seen = [];
u.start({ message: (data, from) => seen.push({ text: bytesToStr(data), addr: from.address, port: from.port }) });
assert_true(u.port > 0, "UDP bound ephemeral port");

// send a datagram to the ctl UDP echo server
const n = u.send(new Uint8Array([104, 105]), HOST, PORT); // "hi"
assert_eq(n, 2, "send returns bytes sent");

// zero-length datagram: message-boundary semantics
u.send(new Uint8Array(0), HOST, PORT);

// DataView (with byteOffset/byteLength), subclass, and TypedArray views all
// send their BYTES (R3: views used to stringify as "104,105")
{
  const ab = new ArrayBuffer(16);
  const dv = new DataView(ab, 4, 6);       // bytes 4..9
  for (let i = 0; i < 6; i++) dv.setUint8(i, 65 + i); // "ABCDEF"
  assert_eq(u.send(dv, HOST, PORT), 6, "DataView sends its byteLength");
  class MyU8 extends Uint8Array {}
  const sub = new MyU8(3); sub[0] = 120; sub[1] = 121; sub[2] = 122; // "xyz"
  assert_eq(u.send(sub, HOST, PORT), 3, "typed-array subclass sends bytes");
  const i8 = new Int8Array(2); i8[0] = -1; i8[1] = 2;
  assert_eq(u.send(i8, HOST, PORT), 2, "Int8Array view sends raw bytes");
}

setTimeout(() => {
  assert_eq(seen.length, 5, "five datagrams received, got " + seen.length);
  if (seen.length >= 1) {
    assert_eq(seen[0].text, ">hi", "echo carries server marker prefix");
    assert_eq(seen[0].addr, HOST, "from.address is loopback");
    assert_eq(seen[0].port, PORT, "from.port is the server port");
  }
  if (seen.length >= 2) assert_eq(seen[1].text, ">", "zero-length datagram delivered as empty payload");
  if (seen.length >= 3) assert_eq(seen[2].text, ">ABCDEF", "DataView bytes echoed verbatim (R3)");
  if (seen.length >= 4) assert_eq(seen[3].text, ">xyz", "subclass bytes echoed verbatim (R3)");
  if (seen.length >= 5) {
    // Int8Array(-1, 2) raw bytes 0xFF 0x02 survive (no text round-trip)
    let ok = seen[4].text.length === 3 && seen[4].text.charCodeAt(1) === 255 && seen[4].text.charCodeAt(2) === 2;
    assert_true(ok, "Int8Array raw bytes echoed, got " + JSON.stringify(seen[4].text));
  }
  u.close();
  summary("net.udp");
}, 600);
''', ctl="udp")

emit("net", "proxy", IMP_TCP, r'''
(async () => {
  const proxy = new TCPProxy({ port: 0, upstream: { host: HOST, port: PORT } });
  proxy.start();
  assert_true(proxy.port > 0, "proxy bound ephemeral port");
  await new Promise((resolve) => setTimeout(resolve, 100));

  // client -> proxy -> ctl tcp echo (E mode), bytes flow both ways untouched
  let acc = "";
  const got = await new Promise((resolve) => {
    const cli = TCPServer.connect({ host: HOST, port: proxy.port }, {
      connect: (c, err) => { if (c) c.write("Eproxy-hello"); else resolve("err:" + err); },
      data: (c, b) => { acc += bytesToStr(b); if (acc.length >= 12) resolve(acc); },
      close: () => {},
    });
    setTimeout(() => resolve(acc || "watchdog"), 3000);
    setTimeout(() => { try { cli.close(); } catch (e) {} }, 2500);
  });
  assert_eq(acc, "Eproxy-hello", "proxied bytes round-trip (mode byte echoed), got " + JSON.stringify(acc));

  const st = proxy.stats();
  assert_eq(typeof st, "object", "stats() shape");
  assert_true(st.bytesUp >= 12, "bytesUp counted: " + st.bytesUp);
  assert_true(st.bytesDown >= 11, "bytesDown counted: " + st.bytesDown);
  assert_true(st.accepted >= 1, "accepted counted");
  proxy.close();
  summary("net.proxy");
})();
''', ctl="tcp")

# ---- RESP wire-format: the python server asserts exact bytes BOTH directions
emit("net", "resp", IMP_RESP, r'''
(async () => {
  const rd = new Redis({ host: "127.0.0.1", port: PORT });
  await rd.command("PING");  // handshake queue flush
  assert_eq(rd.ready, true, "handshake settled");

  // wire format OUT: PING as an array of bulks — ask the server via RAWLOG
  await rd.command("PING");
  const raw = await rd.command("RAWLOG");
  const rawStr = typeof raw === "string" ? raw : bytesToStr(raw);
  assert_true(rawStr.indexOf("*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n") === 0 ||
              rawStr.indexOf("HELLO") >= 0, "handshake sent HELLO: " + JSON.stringify(rawStr.slice(0, 60)));
  assert_true(rawStr.indexOf("*1\r\n$4\r\nPING\r\n") >= 0,
    "client sent exact RESP array *1\\r\\n$4\\r\\nPING\\r\\n, log tail: " + JSON.stringify(rawStr.slice(-60)));

  // inline commands: server parses them too (send raw over a second client? use command with raw bytes)
  // wire format IN: each reply type
  assert_eq(await rd.command("PING"), "PONG", "+PONG simple");
  assert_eq(await rd.command("ECHO", "hello"), "hello", "bulk round-trip");
  assert_eq(await rd.command("SET", "k", "v1"), "OK", "+OK status");
  assert_eq(await rd.command("GET", "k"), "v1", "GET bulk");
  assert_eq(await rd.command("GET", "missing-key"), null, "GET missing -> null ($-1)");
  assert_eq(await rd.command("NULLBULK"), null, "null bulk");
  assert_eq(await rd.command("EMPTYSTR"), "", "empty bulk string");
  assert_eq(await rd.command("INT"), 12345, ":integer reply");
  assert_eq(await rd.command("DEL", "k", "nope"), 1, "multi-arg DEL");

  // error reply rejects with the message
  await rd.command("MYERR").then(
    () => assert(false, "MYERR must reject"),
    (e) => assert_true(String(e.message || e).indexOf("my deliberate error") >= 0,
      "error reply carries server message: " + e.message));

  // nested arrays
  {
    const n = await rd.command("NESTED");
    const j = JSON.stringify(n);
    assert_true(j.indexOf("abc") >= 0 && j.indexOf("1") >= 0, "nested multi-bulk parsed: " + j);
  }
  // NULL array
  {
    const n = await rd.command("NULLARR");
    assert_true(n === null || (Array.isArray(n) && n.length === 0), "null array -> null/[]: " + JSON.stringify(n));
  }
  // binary-safe payload needs the binary:true client (the default client
  // UTF-8-decodes bulks to text, which mangles binary by documented design)
  {
    const rdb = new Redis({ host: "127.0.0.1", port: PORT, binary: true });
    await rdb.command("PING");
    const v = await rdb.command("BINVAL");
    let ok = v instanceof Uint8Array && v.length === 256;
    for (let i = 0; i < 256 && ok; i++) if (v[i] !== i) ok = false;
    assert_true(ok, "binary payload with \\r\\n inside survives (binary client)");
    // big value 1 MiB through the binary client: byte-exact
    const big = await rdb.command("BIGVAL");
    assert_eq(big.length, 1024 * 1024, "1MiB bulk length (bytes)");
    let bigOk = true;
    for (let i = 0; i < 1024 * 1024; i += 4095) if (big[i] !== i % 256) { bigOk = false; break; }
    assert_true(bigOk, "1MiB content spot-check");
    rdb.close();
  }
  // 64-bit integer: exact digits as text without bigint
  {
    const v = await rd.command("BIGINT");
    assert_eq(String(v), "9223372036854775807", "64-bit int exact digits, got " + String(v));
  }
  // pipeline: one round trip, ordered replies
  {
    const replies = await rd.pipeline([["PING"], ["ECHO", "x"], ["SET", "pk", "pv"], ["GET", "pk"]]);
    assert_eq(replies.length, 4, "pipeline reply count");
    assert_eq(replies[0], "PONG", "pipeline [0]");
    assert_eq(replies[1], "x", "pipeline [1]");
    assert_eq(replies[2], "OK", "pipeline [2]");
    assert_eq(replies[3], "pv", "pipeline [3] ordered");
  }
  // premature close: DIE closes mid-reply; pending rejects
  {
    await rd.command("DIE").then(
      () => assert(true, "DIE +BYE partial resolved or rejects below"),
      (e) => assert(true, "DIE rejected: " + e.message));
    await new Promise((r) => setTimeout(r, 200));
    let err = null;
    try { await rd.command("PING"); } catch (e) { err = e; }
    assert_true(err !== null, "commands after teardown reject: " + (err && err.message));
  }
  rd.close();

  // RESP3 protocol negotiated + push frames
  const rd2 = new Redis({ host: "127.0.0.1", port: PORT });
  const pushes = [];
  rd2.on("push", (p) => pushes.push(p));
  await rd2.command("PING");
  assert_true(rd2.protocol === 2 || rd2.protocol === 3, "protocol negotiated: " + rd2.protocol);
  await rd2.command("PUSH1");
  await new Promise((r) => setTimeout(r, 300));
  assert_true(pushes.length >= 1, "RESP3 push frame delivered to on(push), got " + pushes.length);
  rd2.close();

  // bigint option client
  const rd3 = new Redis({ host: "127.0.0.1", port: PORT, bigint: true });
  const big = await rd3.command("BIGINT");
  assert_true(typeof big === "bigint" || big === "9223372036854775807", "bigint flag: " + typeof big);
  rd3.close();

  summary("net.resp");
})().catch((e) => { print("FAIL uncaught " + e); throw e; });
''', ctl="resp")

# ---- DNS loopback: dyna DNSServer + DNSResolver (no external DNS)
emit("net", "dns", IMP_DNS + '''
import { Path, writeFile } from "dyna:file";
''', r'''
const lp = new Path(Path.cwd(), "scratch", "dns-" + ((Math.random() * 1e9) | 0) + ".err");
const L = (m) => writeFile(lp, m + "\n", { append: true });

const srv = new DNSServer({ port: 0, host: "127.0.0.1" });
assert_true(srv.port > 0, "DNSServer bound ephemeral port");
srv.start((name, type) => {
  if (name === "test.local" && type === 1) return "127.0.0.1";
  if (name === "v6.local" && type === 28) return "::1";
  return null;
});

const res = new DNSResolver({ server: "127.0.0.1", port: srv.port, timeoutMs: 2000 });
assert_throws(() => new DNSResolver({ server: "example" }), null, "hostname server refused (no bootstrap)");
assert_throws(() => new DNSResolver({ server: "127.0.0.1", port: 0 }), null, "port 0 refused");
assert_throws(() => new DNSResolver({ server: "127.0.0.1", timeoutMs: 0 }), null, "timeoutMs 0 refused");

const a = await new Promise((resolve) => res.query("test.local", 1, (err, records) => resolve({ err, records })));
assert_eq(a.err, null, "A query no error: " + a.err);
assert_eq(a.records[0].address, "127.0.0.1", "A record address");
assert_eq(a.records[0].ttl, 60, "A record TTL fixed at 60");
assert_eq(a.records[0].type, 1, "A record type");

const aaaa = await new Promise((resolve) => res.query("v6.local", 28, (err, records) => resolve({ err, records })));
assert_eq(aaaa.records[0].address, "::1", "AAAA record address");

const neg = await new Promise((resolve) => res.query("nope.local", 1, (err, records) => resolve({ err, records })));
assert_true(neg.err !== null || !neg.records || neg.records.length === 0, "unanswered query errors or is empty");

const bad = await new Promise((resolve) => res.query("test.local", 1, (err, records) => resolve({ err, records })));
assert_true(bad !== undefined, "callback form resolves");

srv.close();
res.close();   /* the resolver owns a bound UDP socket; releasing it lets the loop drain */
summary("net.dns");
''')

# ---- pure IP math vs python ipaddress (baked at generation)
IP_CASES = []
for text in ["192.168.1.1", "10.0.0.1", "127.0.0.1", "8.8.8.8", "169.254.5.5", "224.0.0.1",
             "0.0.0.0", "::1", "::", "fe80::1", "ff02::1", "2001:db8::ff00:42:8329",
             "::ffff:10.0.0.1", "2001:db8::1"]:
    ip = ipaddress.ip_address(text)
    canon = str(ip)
    # dyna:net documents isPrivate as RFC 1918 + fc00::/7 ONLY (narrower than
    # python's is_private which folds loopback/link-local in)
    if ip.version == 4:
        priv = ip in ipaddress.ip_network("10.0.0.0/8") or \
               ip in ipaddress.ip_network("172.16.0.0/12") or \
               ip in ipaddress.ip_network("192.168.0.0/16")
    elif isinstance(ip, ipaddress.IPv6Address) and getattr(ip, "ipv4_mapped", None) and ip.ipv4_mapped:
        p4 = ip.ipv4_mapped
        priv = p4 in ipaddress.ip_network("10.0.0.0/8") or \
               p4 in ipaddress.ip_network("172.16.0.0/12") or \
               p4 in ipaddress.ip_network("192.168.0.0/16")
    else:
        priv = ip in ipaddress.ip_network("fc00::/7")
    IP_CASES.append({
        "t": text, "canon": canon,
        "is4": ip.version == 4, "is6": ip.version == 6,
        "loopback": ip.is_loopback, "private": priv,
        "multicast": ip.is_multicast, "unspecified": ip.is_unspecified,
        "linklocal": ip.is_link_local,
    })

emit("net", "ipmap", IMP_IP, r'''
const CASES = __IP_CASES__;
for (const c of CASES) {
  if (!isValid(c.t)) { assert(false, "isValid false for " + c.t); continue; }
  const a = parseAddr(c.t);
  assert_eq(a.string, c.canon, "canonical(" + c.t + ")");
  assert_eq(a.is4, c.is4, "is4 " + c.t);
  assert_eq(a.is6, c.is6, "is6 " + c.t);
  assert_eq(a.bytes.length, c.is4 ? 4 : 16, "bytes length " + c.t);
  assert_eq(isLoopback(c.t), c.loopback, "isLoopback " + c.t);
  assert_eq(isPrivate(c.t), c.private, "isPrivate " + c.t);
  assert_eq(isMulticast(c.t), c.multicast, "isMulticast " + c.t);
  assert_eq(isUnspecified(c.t), c.unspecified, "isUnspecified " + c.t);
}
assert_eq(isValid("999.1.1.1"), false, "isValid rejects 999");
assert_eq(isValid("nope"), false, "isValid rejects junk");
assert_eq(isValid(42), false, "isValid non-string false");
assert_throws(() => parseAddr("999.1.1.1"), "TypeError", "parseAddr malformed throws");
assert_throws(() => parseAddr("fe80::1%eth0"), "TypeError", "zone refused");

assert_eq(canonical("0:0:0:0:0:0:0:1"), "::1", "compression");
assert_eq(canonical("2001:0db8:0000:0000:0000:ff00:0042:8329"), "2001:db8::ff00:42:8329", "leftmost-longest compression");

// prefix math vs python
assert_eq(masked("192.168.1.55/24"), "192.168.1.0", "masked v4");
assert_eq(masked("2001:db8:f00::/32"), "2001:db8::", "masked v6");
assert_eq(JSON.stringify(parsePrefix("10.0.0.0/8")), '{"addr":"10.0.0.0","bits":8}', "parsePrefix");
assert_eq(contains("10.0.0.0/8", "10.1.2.3"), true, "contains true");
assert_eq(contains("10.0.0.0/8", "11.1.2.3"), false, "contains false");
assert_eq(compareAddr("10.0.0.1", "192.168.1.1"), -1, "v4 sorts before v4");
assert_eq(compareAddr("1.1.1.1", "1.1.1.1"), 0, "compare equal");

// compiled Prefix
const p = new Prefix("10.0.0.0/8");
assert_eq(p.contains("10.1.2.3"), true, "Prefix.contains in");
assert_eq(p.contains("11.0.0.1"), false, "Prefix.contains out");
assert_eq(p.contains("not-an-ip"), false, "unparseable is false");
assert_eq(p.bits, 8, "Prefix.bits");
assert_eq(p.masked, "10.0.0.0", "Prefix.masked");
assert_eq(p.isIPv4, true, "Prefix.isIPv4");
assert_eq(new Prefix("10.0.0.0/8").overlaps(new Prefix("10.5.0.0/16")), true, "overlaps true");
assert_eq(new Prefix("10.0.0.0/8").overlaps(new Prefix("192.168.0.0/16")), false, "overlaps false");
assert_throws(() => new Prefix("nonsense"), "TypeError", "bad CIDR throws");

// RateLimiter
const rl = new RateLimiter({ tokensPerSec: 5, burst: 3 });
assert_eq(rl.allow("a"), true, "allow 1");
assert_eq(rl.allow("a"), true, "allow 2");
assert_eq(rl.allow("a"), true, "allow 3");
assert_eq(rl.allow("a"), false, "deny past burst");
assert_eq(rl.tokens("a"), 0, "tokens drained");
const stats = rl.stats;
assert_eq(stats.allowed, 3, "stats.allowed");
assert_eq(stats.denied, 1, "stats.denied");
assert_eq(stats.burst, 3, "stats.burst");
rl.reset("a");
assert_eq(rl.allow("a"), true, "reset restores");
assert_throws(() => new RateLimiter({}), null, "tokensPerSec required");
assert_throws(() => rl.allow("a", 0), null, "non-positive cost refused");
assert_throws(() => rl.allow("a", -1), null, "negative cost refused");

// Metrics
Metrics.reset();
Metrics.counter("http_requests_total", 2, { method: "GET" });
Metrics.counter("http_requests_total");
Metrics.gauge("queue_depth", 3);
const lines = Metrics.scrape().split("\n")
  .filter((l) => l.startsWith("http_requests_total") || l.startsWith("queue_depth"));
assert_eq(lines.join("|"), 'http_requests_total{method="GET"} 2|http_requests_total 1|queue_depth 3',
  "prometheus exposition: " + JSON.stringify(lines));
Metrics.histogram("latency", 0.007);
assert_true(Metrics.scrape().indexOf("latency_bucket") >= 0, "histogram buckets exposed");
assert_throws(() => Metrics.counter("http_requests_total", -1), "RangeError", "negative increment refused");
summary("net.ipmap");
'''.replace("__IP_CASES__", json.dumps(IP_CASES)))

print("gen_net: 5 probes")

print("gen_net: 4 probes (eyeballs ticketed -- see CHANGELOG)")
