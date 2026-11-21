// flags: --std
// covers: dyna:postgres.PostgreSQL
// Options maxProtocolVersion/minProtocolVersion/sslNegotiation and the protocolVersion getter. PG 18 protocol 3.2: StartupMessage 196610, NegotiateProtocolVersion downgrade (only before auth, lower minor, no options), BackendKeyData key 4 bytes in 3.0 and 4..256 in 3.2,
// CancelRequest 12+n bytes; direct TLS (PG 17+) sends the ClientHello at once, ALPN "postgresql" in both negotiation styles. cancel() negotiates TLS like the session (libpq 17), never falls back to plaintext and refuses an 'S' with bytes stuffed behind it.
// maxProtocolVersion defaults to "3.0" (libpq 18). Unreachable by test: the "selected an ALPN protocol other than 'postgresql'" branch (OpenSSL servers select only an offered protocol).
// Unreachable by test: the cancel connection's direct-TLS "no ALPN" refusal (the session itself fails first on a server without ALPN).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_proto32");
import { ensurePostgres, stopStarted, testCert } from "../lib/services.js";
import { TCPServer } from "dyna:net";
import { PostgreSQL } from "dyna:postgres";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const within = (p, ms = 6000) => Promise.race([p, sleep(ms).then(() => { throw new Error("case timed out after " + ms + " ms"); })]);
async function settle(p) { try { return { ok: await p }; } catch (e) { return { err: e }; } }
const em = (r) => r && r.err ? String(r.err.message || r.err) : "(resolved)";
const str = (u) => { let s = ""; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };
const bin = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };
const i32 = (v) => String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
const be = (s, o) => (s.charCodeAt(o) << 24 | s.charCodeAt(o + 1) << 16 | s.charCodeAt(o + 2) << 8 | s.charCodeAt(o + 3)) >>> 0;
const msg = (t, b) => t + i32(b.length + 4) + b;
const npv = (minor, opts = []) => msg("v", i32(minor) + i32(opts.length) + opts.map((o) => o + "\0").join(""));
const key = (k) => msg("K", i32(4242) + k);
const ready = msg("R", i32(0)) + msg("S", "server_version\x0018.1\0") + key("abcd") + msg("Z", "I");
function syncThrow(f) { try { const c = f(); if (c && c.close) c.close(); return null; } catch (e) { return e; } }

function fake(onStart, srvOpts = {}) {
  const bufs = new Map(), st = new Map(), log = { starts: [], cancels: [], raw: [], hello: [] };
  const srv = new TCPServer({ port: 0, ...srvOpts });
  srv.start({
    data: (c, b) => {
      const chunk = str(b);
      if (!st.get(c) && !bufs.get(c)) log.raw.push(chunk);
      if (st.get(c) === 3) { log.hello.push(chunk); return; }
      bufs.set(c, (bufs.get(c) || "") + chunk);
      for (;;) {
        const s = bufs.get(c);
        if (!st.get(c)) {
          if (s.length < 8) return;
          const len = be(s, 0);
          if (len > 10000) return;
          if (s.length < len) return;
          const code = be(s, 4);
          bufs.set(c, s.slice(len));
          if (code === 80877102) { log.cancels.push(s.slice(0, len)); st.set(c, 2); try { c.close(); } catch (e) {} return; }
          if (code === 80877103) { st.set(c, 3); c.write(bin("S")); return; }
          st.set(c, 1); log.starts.push(code);
          c.write(bin(onStart(code)));
          continue;
        }
        if (s.length < 5) return;
        const len = be(s, 1);
        if (s.length < len + 1) return;
        const t = s[0];
        bufs.set(c, s.slice(len + 1));
        if (t === "Q") try { c.write(bin(msg("C", "SELECT 0\0") + msg("Z", "I"))); } catch (e) {}
      }
    },
    close: (c) => { bufs.delete(c); st.delete(c); },
  });
  return { srv, log };
}
function starttlsFront(backPort, denyAfter = 99, stuffAfter = 99) {
  const log = { ssl: 0, plain: [], closed: 0 }, st = new Map();
  const srv = new TCPServer({ port: 0 });
  srv.start({
    data: (c, b) => {
      let s = st.get(c);
      if (!s) { s = { buf: "", up: null, pend: [], mode: 0 }; st.set(c, s); }
      if (s.mode === 2) { if (s.up) s.up.write(b); else s.pend.push(b); return; }
      if (s.mode === 3) return;
      s.buf += str(b);
      if (s.buf.length < 8) return;
      if (be(s.buf, 4) !== 80877103) { log.plain.push(s.buf); s.mode = 3; return; }
      log.ssl++;
      if (log.ssl > denyAfter) { s.buf = s.buf.slice(8); try { c.write(bin("N")); } catch (e) {} return; }
      c.write(bin(log.ssl > stuffAfter ? "S\x16\x03\x01stuffed" : "S")); s.mode = 2;
      if (s.buf.length > 8) s.pend.push(bin(s.buf.slice(8)));
      TCPServer.connect({ host: "127.0.0.1", port: backPort }, {
        connect: (up) => { if (!up) { try { c.close(); } catch (e) {} return; } s.up = up; for (const p of s.pend) up.write(p); s.pend = []; },
        data: (up, d) => { try { c.write(d); } catch (e) {} },
        close: () => { try { c.close(); } catch (e) {} },
      });
    },
    close: (c) => { log.closed++; const s = st.get(c); if (s && s.up) try { s.up.close(); } catch (e) {} st.delete(c); },
  });
  return { srv, log };
}
const conn = (srv, o) => new PostgreSQL({ host: "127.0.0.1", port: srv.port, user: "u", password: "pencil", connectTimeoutMs: 1500, maxProtocolVersion: "3.2", ...o });

let svc = null;
try {
await __kitS.section("options: values, types, hostile getters, Proxy, NUL", async () => {
  const bad = [
    [{ maxProtocolVersion: "3.1" }, TypeError], [{ maxProtocolVersion: 3.2 }, TypeError], [{ maxProtocolVersion: "3.2\0" }, TypeError],
    [{ minProtocolVersion: "4.0" }, TypeError], [{ minProtocolVersion: "" }, TypeError], [{ sslNegotiation: "Direct" }, TypeError],
    [{ sslNegotiation: "direct\0" }, TypeError], [{ sslNegotiation: { toString() { return "direct"; } } }, TypeError],
    [{ minProtocolVersion: "3.2", maxProtocolVersion: "3.0" }, RangeError], [{ sslNegotiation: "direct" }, TypeError],
  ];
  for (const [o, E] of bad) { const e = syncThrow(() => new PostgreSQL({ host: "127.0.0.1", port: 1, ...o })); check(e instanceof E, JSON.stringify(o) + " -> " + e); }
  const boom = new Error("boom");
  for (const k of ["maxProtocolVersion", "minProtocolVersion", "sslNegotiation"]) {
    const o = { host: "127.0.0.1", port: 1 }; Object.defineProperty(o, k, { get() { throw boom; }, enumerable: true });
    check(syncThrow(() => new PostgreSQL(o)) === boom, k + ": getter throw propagates");
    const p = new Proxy({ host: "127.0.0.1", port: 1, [k]: "3.0" }, { get(t, q) { if (q === k) throw boom; return t[q]; } });
    check(syncThrow(() => new PostgreSQL(p)) === boom, k + ": Proxy get trap throw propagates");
  }
  const c = new PostgreSQL({ host: "127.0.0.1", port: 1, maxProtocolVersion: "3.0", minProtocolVersion: "3.0", sslNegotiation: "postgres", connectTimeoutMs: 200 });
  check(c.protocolVersion === null, "protocolVersion is null before ready: " + c.protocolVersion);
  c.close();
});

await __kitS.section("fake server: StartupMessage version and NegotiateProtocolVersion", async () => {
  {
    const { srv, log } = fake(() => ready);
    const c = conn(srv, { maxProtocolVersion: undefined }); const r = await settle(within(c.query("SELECT 1")));
    check(!r.err && log.starts[0] === 196608 && c.protocolVersion === "3.0", "default requests 3.0 (libpq 18): " + log.starts[0] + " " + c.protocolVersion + " " + em(r));
    c.close(); srv.close();
  }
  for (const [name, o] of [["maxProtocolVersion 3.2", {}], ["minProtocolVersion 3.2 raises the default max", { maxProtocolVersion: undefined, minProtocolVersion: "3.2" }]]) {
    const { srv, log } = fake(() => ready);
    const c = conn(srv, o); const r = await settle(within(c.query("SELECT 1")));
    check(!r.err && log.starts[0] === 196610 && c.protocolVersion === "3.2", name + " requests 196610: " + log.starts[0] + " " + c.protocolVersion + " " + em(r));
    c.close(); srv.close();
  }
  {
    const { srv, log } = fake(() => ready);
    const c = conn(srv, { maxProtocolVersion: "3.0" }); const r = await settle(within(c.query("SELECT 1")));
    check(!r.err && log.starts[0] === 196608 && c.protocolVersion === "3.0", "maxProtocolVersion 3.0 requests 196608: " + log.starts[0] + " " + em(r));
    c.close(); srv.close();
  }
  {
    const { srv } = fake(() => npv(0) + ready);
    const c = conn(srv); const r = await settle(within(c.query("SELECT 1")));
    check(!r.err && c.protocolVersion === "3.0", "downgrade to 3.0 accepted, 4-byte key: " + em(r) + " " + c.protocolVersion);
    c.close(); srv.close();
  }
  const refuse = [
    ["NPV minor 2 is not a downgrade", {}, npv(2) + ready, /not a downgrade/],
    ["NPV minor 1 is undefined", {}, npv(1) + ready, /3\.1/],
    ["NPV naming options never sent", {}, npv(0, ["_pq_.x"]) + ready, /options/],
    ["NPV truncated option list", {}, msg("v", i32(0) + i32(3) + "ab\0"), /malformed NegotiateProtocolVersion/],
    ["NPV huge option count", {}, msg("v", i32(0) + i32(0xffffffff)), /malformed NegotiateProtocolVersion/],
    ["NPV trailing bytes", {}, msg("v", i32(0) + i32(0) + "x") + ready, /malformed NegotiateProtocolVersion/],
    ["NPV shorter than 8 bytes", {}, msg("v", i32(0)), /malformed NegotiateProtocolVersion/],
    ["NPV twice", {}, npv(0) + npv(0) + ready, /out of order/],
    ["NPV after authentication", {}, msg("R", i32(0)) + npv(0) + msg("Z", "I"), /out of order/],
    ["NPV below minProtocolVersion", { minProtocolVersion: "3.2" }, npv(0) + ready, /minProtocolVersion/],
    ["NPV answering a 3.0 request", { maxProtocolVersion: "3.0" }, npv(0) + ready, /not a downgrade/],
    ["3.2 key of 3 bytes", {}, msg("R", i32(0)) + key("abc") + msg("Z", "I"), /4\.\.256/],
    ["3.2 key of 257 bytes", {}, msg("R", i32(0)) + key("k".repeat(257)) + msg("Z", "I"), /4\.\.256/],
    ["BackendKeyData without a key", {}, msg("R", i32(0)) + msg("K", i32(1)) + msg("Z", "I"), /4\.\.256/],
    ["3.0 key of 32 bytes", {}, npv(0) + msg("R", i32(0)) + key("k".repeat(32)) + msg("Z", "I"), /4 bytes in protocol 3\.0/],
    ["3.0 (max) key of 5 bytes", { maxProtocolVersion: "3.0" }, msg("R", i32(0)) + key("kkkkk") + msg("Z", "I"), /4 bytes in protocol 3\.0/],
  ];
  for (const [name, o, reply, re] of refuse) {
    const { srv } = fake(() => reply);
    const c = conn(srv, o); const r = await settle(within(c.query("SELECT 1")));
    check(r.err && re.test(em(r)), name + ": " + em(r));
    c.close(); srv.close();
  }
  for (const kl of [4, 32, 256]) {
    const { srv, log } = fake(() => msg("R", i32(0)) + key("q".repeat(kl - 1) + "Z") + msg("Z", "I"));
    const c = conn(srv); const r = await settle(within(c.query("SELECT 1")));
    c.cancel();
    for (let k = 0; k < 200 && !log.cancels.length; k++) await sleep(5);
    const cr = log.cancels[0] || "";
    check(!r.err && cr.length === 12 + kl && be(cr, 0) === 12 + kl && be(cr, 4) === 80877102 && be(cr, 8) === 4242 && cr.endsWith("Z"),
      "3.2 key of " + kl + " bytes accepted and sent back in a " + (12 + kl) + "-byte CancelRequest: got " + cr.length + " " + em(r));
    c.close(); srv.close();
  }
});

await __kitS.section("fake server: direct TLS and ALPN in the ClientHello", async () => {
  {
    const { srv, log } = fake(() => ready);
    const c = conn(srv, { tls: true, sslNegotiation: "direct" }); const r = await settle(within(c.query("SELECT 1")));
    const h = log.raw[0] || "";
    check(r.err && h.charCodeAt(0) === 0x16 && h.indexOf("postgresql") > 0 && !log.starts.length, "direct: first bytes are a TLS ClientHello with ALPN postgresql, no SSLRequest: " + h.charCodeAt(0) + " " + em(r));
    c.close(); srv.close();
  }
  {
    const { srv, log } = fake(() => ready);
    const c = conn(srv, { tls: true }); const r = await settle(within(c.query("SELECT 1")));
    for (let k = 0; k < 100 && !log.hello.length; k++) await sleep(5);
    const h = log.hello.join("");
    check(r.err && be(log.raw[0] || "\0\0\0\0\0\0\0\0", 4) === 80877103 && h.charCodeAt(0) === 0x16 && h.indexOf("postgresql") > 0, "postgres: SSLRequest then a ClientHello with ALPN postgresql: " + em(r));
    c.close(); srv.close();
  }
});

await __kitS.section("cancel over TLS (libpq 17): direct and SSLRequest, no plaintext fallback", async () => {
  const rsa = testCert();
  check(rsa, "openssl makes a test certificate");
  if (!rsa) return;
  const tlsOpts = { tls: { cert: rsa.cert, key: rsa.key, alpn: "postgresql" } };
  const k32 = "c".repeat(31) + "Z";
  const replyK = (k) => () => msg("R", i32(0)) + key(k) + msg("Z", "I");
  const waitFor = async (f) => { for (let k = 0; k < 300 && !f(); k++) await sleep(5); return f(); };
  {
    const { srv, log } = fake(replyK(k32), tlsOpts);
    const c = conn(srv, { tls: true, ca: rsa.ca, sslNegotiation: "direct" });
    const r = await settle(within(c.query("SELECT 1")));
    c.cancel(); await waitFor(() => log.cancels.length);
    const cr = log.cancels[0] || "";
    check(!r.err && cr.length === 44 && be(cr, 4) === 80877102 && be(cr, 8) === 4242 && cr.endsWith("Z"), "direct: the CancelRequest arrives inside TLS with the 32-byte key: " + cr.length + " " + em(r));
    c.close(); srv.close();
  }
  for (const [name, o, k] of [["SSLRequest, 3.2 key", {}, k32], ["SSLRequest, 3.0 key", { maxProtocolVersion: undefined }, "abcd"]]) {
    const back = fake(replyK(k), tlsOpts), front = starttlsFront(back.srv.port);
    const c = conn(front.srv, { tls: true, ca: rsa.ca, ...o });
    const r = await settle(within(c.query("SELECT 1")));
    c.cancel(); await waitFor(() => back.log.cancels.length);
    const cr = back.log.cancels[0] || "";
    check(!r.err && front.log.ssl === 2 && !front.log.plain.length && cr.length === 12 + k.length && be(cr, 8) === 4242 && cr.endsWith(k),
      name + ": cancel sends SSLRequest, then the CancelRequest inside TLS: ssl " + front.log.ssl + " plain " + front.log.plain.length + " len " + cr.length + " " + em(r));
    c.close(); front.srv.close(); back.srv.close();
  }
  {
    const back = fake(replyK("abcd"), tlsOpts), front = starttlsFront(back.srv.port, 1);
    const c = conn(front.srv, { tls: true, ca: rsa.ca });
    const r = await settle(within(c.query("SELECT 1")));
    c.cancel(); await waitFor(() => front.log.closed >= 1);
    check(!r.err && front.log.ssl === 2 && !front.log.plain.length && !back.log.cancels.length, "N to the cancel's SSLRequest: no plaintext fallback: ssl " + front.log.ssl + " plain " + front.log.plain.length + " " + em(r));
    c.close(); front.srv.close(); back.srv.close();
  }
  {
    const back = fake(replyK("abcd"), tlsOpts), front = starttlsFront(back.srv.port, 99, 1);
    const c = conn(front.srv, { tls: true, ca: rsa.ca });
    const r = await settle(within(c.query("SELECT 1")));
    c.cancel(); await waitFor(() => front.log.closed >= 1);
    check(!r.err && front.log.ssl === 2 && !front.log.plain.length && !back.log.cancels.length,
      "'S' with bytes stuffed behind it (CVE-2021-23222 class): the cancel stops, nothing reaches the server: ssl " + front.log.ssl + " plain " + front.log.plain.length + " cancels " + back.log.cancels.length + " " + em(r));
    c.close(); front.srv.close(); back.srv.close();
  }
});

await __kitS.section("TLS fake server: ALPN selection, PLUS with an unusable certificate hash", async () => {
  const ed = testCert({ alg: "ed25519" }), rsa = testCert();
  check(ed && rsa, "openssl makes Ed25519 and RSA test certificates");
  if (!ed || !rsa) return;
  const plus = msg("R", i32(10) + "SCRAM-SHA-256-PLUS\0SCRAM-SHA-256\0\0");
  const cases = [
    ["direct, server offers only ALPN h2 (OpenSSL selects nothing)", rsa, "h2", {}, ready, /select ALPN 'postgresql'|other than 'postgresql'/, false],
    ["direct, server selects no ALPN", rsa, null, {}, ready, /needs the server to select ALPN/, false],
    ["prefer + PLUS offered + Ed25519 certificate (owner: fail like libpq)", ed, "postgresql", {}, plus, /'prefer'.*no usable hash/, false],
    ["require + PLUS offered + Ed25519 certificate", ed, "postgresql", { channelBinding: "require" }, plus, /'require'.*no usable hash/, false],
    ["disable + PLUS offered + Ed25519 certificate still sends n,,", ed, "postgresql", { channelBinding: "disable" }, plus, /./, true],
  ];
  for (const [name, cert, alpn, o, reply, re, sends] of cases) {
    let got = "", started = false;
    const srv = new TCPServer({ port: 0, tls: { cert: cert.cert, key: cert.key, ...(alpn ? { alpn } : {}) } });
    srv.start({ data: (c, b) => { got += str(b); if (!started && got.length >= 8 && got.length >= be(got, 0)) { started = true; try { c.write(bin(reply)); } catch (e) {} } }, close: () => {} });
    const c = conn(srv, { tls: true, ca: cert.ca, sslNegotiation: "direct", ...o });
    const r = await settle(within(c.query("SELECT 1")));
    check(r.err && re.test(em(r)) && (got.indexOf("n,,n=,r=") >= 0) === sends, name + ": " + em(r) + " (SASLInitialResponse sent: " + (got.indexOf("n,,n=,r=") >= 0) + ")");
    c.close(); srv.close();
  }
});

svc = __kitS.listing ? null : await ensurePostgres({ tls: true });
await __kitS.section("live PostgreSQL: 3.2 by default, 3.0 on request, direct TLS, cancel", async () => {
  if (!svc || !svc.ok || !svc.ca) { print("test_postgres_postgre_sql_proto32: SKIP live part: " + (svc ? svc.why || "no CA" : "listing")); return; }
  const base = { ...svc.conn, connectTimeoutMs: 5000 };
  const ver = Number((await (async () => { const c = new PostgreSQL(base); try { return (await c.query("SHOW server_version_num")).rows[0].server_version_num; } finally { c.close(); } })()));
  const v32 = ver >= 180000 ? "3.2" : "3.0";
  const cases = [["plain default", {}, "3.0"], ["plain 3.2", { maxProtocolVersion: "3.2" }, v32],
    ["tls default", { tls: true, ca: svc.ca }, "3.0"], ["direct tls 3.2", { tls: true, ca: svc.ca, sslNegotiation: "direct", maxProtocolVersion: "3.2" }, v32],
    ["direct tls 3.2 + channelBinding require", { tls: true, ca: svc.ca, sslNegotiation: "direct", channelBinding: "require", maxProtocolVersion: "3.2" }, v32]];
  if (ver < 170000) cases.splice(3, 2);
  for (const [name, o, want] of cases) {
    const c = new PostgreSQL({ ...base, ...o });
    const r = await settle(c.query("SELECT 1 AS one"));
    check(!r.err && r.ok.rows[0].one === 1 && c.protocolVersion === want, name + ": " + em(r) + " protocol " + c.protocolVersion + " want " + want);
    const q = settle(c.query("SELECT pg_sleep(5)"));
    await sleep(150); c.cancel();
    // wall-clock: pg_sleep(5) runs 5 s unless the cancel lands, so a 4 s deadline is what proves the interrupt; it is a hang bound, not a speed budget
    const t0 = Date.now(), rq = await q;
    check(rq.err && /cancel/i.test(em(rq)) && Date.now() - t0 < 4000, name + ": cancel with the " + want + " key interrupts pg_sleep: " + em(rq));
    c.close();
  }
  if (ver >= 180000) {
    const c = new PostgreSQL({ ...base, minProtocolVersion: "3.2" });
    const r = await settle(within(c.query("SELECT 1"))); check(!r.err && c.protocolVersion === "3.2", "minProtocolVersion 3.2 on PG 18: " + em(r)); c.close();
  }
});
} finally {
  stopStarted();
}
print(`test_postgres_postgre_sql_proto32: ${n - fails}/${n} passed`);
if (fails) throw new Error(fails + " failed");
