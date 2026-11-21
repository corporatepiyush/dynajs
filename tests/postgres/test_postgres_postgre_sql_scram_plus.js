// flags: --std
// covers: dyna:postgres.PostgreSQL
// The channelBinding option. RFC 5802 s.6 gs2 flags n/y/p, RFC 5929 tls-server-end-point, PG 54.3.1: PLUS is offered only over SSL; libpq channel_binding semantics. PBKDF2 runs off the JS thread.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_scram_plus");
import * as std from "std";
import { ensurePostgres, stopStarted } from "../lib/services.js";
import { TCPServer } from "dyna:net";
import { PostgreSQL } from "dyna:postgres";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function settle(p) { try { return { ok: await p }; } catch (e) { return { err: e }; } }
const em = (r) => r && r.err ? String(r.err.message || r.err) : "(resolved)";
function syncThrow(f) { try { const c = f(); if (c && c.close) c.close(); return null; } catch (e) { return e; } }
const str = (u) => { let s = ""; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };
const bin = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };
const i32 = (v) => String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
const msg = (t, b) => t + i32(b.length + 4) + b;
const b64 = (s) => { const A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; let o = ""; for (let i = 0; i < s.length; i += 3) { const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2); const v = (a << 16) | ((b || 0) << 8) | (c || 0); o += A[v >> 18] + A[(v >> 12) & 63] + (i + 1 < s.length ? A[(v >> 6) & 63] : "=") + (i + 2 < s.length ? A[v & 63] : "="); } return o; };

function fake(onAuth) {
  const bufs = new Map(), st = new Map(), log = [];
  const srv = new TCPServer({ port: 0 });
  srv.start({
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + str(b));
      for (;;) {
        const s = bufs.get(c);
        if (!st.get(c)) {
          if (s.length < 8) return;
          const len = (s.charCodeAt(0) << 24 | s.charCodeAt(1) << 16 | s.charCodeAt(2) << 8 | s.charCodeAt(3)) >>> 0;
          if (s.length < len) return;
          bufs.set(c, s.slice(len)); st.set(c, 1);
          c.write(bin(onAuth("start", "", log)));
          continue;
        }
        if (s.length < 5) return;
        const len = (s.charCodeAt(1) << 24 | s.charCodeAt(2) << 16 | s.charCodeAt(3) << 8 | s.charCodeAt(4)) >>> 0;
        if (s.length < len + 1) return;
        const t = s[0], body = s.slice(5, len + 1);
        bufs.set(c, s.slice(len + 1));
        log.push(t + ":" + body);
        const out = onAuth(t, body, log);
        if (out) try { c.write(bin(out)); } catch (e) {}
      }
    },
    close: (c) => { bufs.delete(c); st.delete(c); },
  });
  return { srv, log };
}
const conn = (srv, o) => new PostgreSQL({ host: "127.0.0.1", port: srv.port, user: "u", password: "pencil", connectTimeoutMs: 3000, ...o });

let svc = null;
try {
await __kitS.section("fake server: forged server-final while PBKDF2 runs", async () => {
  const z = "A".repeat(43) + "=";
  let sawQ = false;
  const { srv } = fake((t, body) => {
    if (t === "start") return msg("R", i32(10) + "SCRAM-SHA-256\0\0");
    if (t === "p" && body.indexOf("SCRAM-SHA-256\0") === 0)
      return msg("R", i32(11) + "r=" + body.slice(body.indexOf(",r=") + 3) + "srv,s=QSXCR+Q6sek8bf92,i=1000000") + msg("R", i32(12) + "v=" + z) + msg("R", i32(0)) + msg("Z", "I");
    if (t === "Q") { sawQ = true; return msg("C", "SELECT 0\0") + msg("Z", "I"); }
    return "";
  });
  const c = conn(srv);
  const r = await settle(c.query("SELECT 1"));
  c.close(); srv.close();
  check(r.err && !sawQ, "R(11) i=1e6 + R(12) v=<32 zero bytes> + R(0) + Z in one write must fail before any query: " + em(r));
});
await __kitS.section("fake server: downgrade and require refusals (plaintext)", async () => {
  const cases = [
    ["PLUS offered without TLS", {}, msg("R", i32(10) + "SCRAM-SHA-256-PLUS\0SCRAM-SHA-256\0\0"), /SCRAM-SHA-256-PLUS over a connection without TLS/],
    ["require + AuthenticationOk without SASL", { channelBinding: "require" }, msg("R", i32(0)) + msg("Z", "I"), /require.*without SCRAM-SHA-256-PLUS/],
    ["require + cleartext", { channelBinding: "require", insecureAuth: true }, msg("R", i32(3)), /require.*password and MD5/],
    ["require + MD5", { channelBinding: "require", insecureAuth: true }, msg("R", i32(5) + "salt"), /require.*password and MD5/],
    ["require + SCRAM without TLS", { channelBinding: "require" }, msg("R", i32(10) + "SCRAM-SHA-256\0\0"), /require.*needs tls: true/],
    ["disable + only PLUS (plaintext refusal first)", { channelBinding: "disable" }, msg("R", i32(10) + "SCRAM-SHA-256-PLUS\0\0"), /without TLS/],
  ];
  for (const [name, o, reply, re] of cases) {
    const { srv } = fake((t) => t === "start" ? reply : "");
    const c = conn(srv, o);
    const r = await settle(c.query("SELECT 1"));
    check(r.err && re.test(em(r)), name + ": " + em(r));
    c.close(); srv.close();
  }
});

await __kitS.section("fake server: plaintext SCRAM sends gs2 n,, and c=biws; PBKDF2 off-thread with GC in flight", async () => {
  let cnonce = "";
  const { srv, log } = fake((t, body) => {
    if (t === "start") return msg("R", i32(10) + "SCRAM-SHA-256-PLUS\0SCRAM-SHA-256\0\0".slice(19));
    if (t === "p" && body.startsWith("SCRAM-SHA-256\0")) {
      const cf = body.slice(18);
      cnonce = /r=([^,]*)/.exec(cf)[1];
      return msg("R", i32(11) + "r=" + cnonce + "srv,s=" + b64("saltsaltsalt") + ",i=200000");
    }
    if (t === "p") return msg("E", "SFATAL\0C28P01\0Mstop here\0\0");
    return "";
  });
  let ticks = 0;
  const iv = setInterval(() => { ticks++; std.gc(); }, 1);
  const r = await settle(conn(srv, {}).query("SELECT 1"));
  clearInterval(iv);
  const init = log.find((x) => x.startsWith("p:SCRAM")) || "";
  const fin = log.filter((x) => x.startsWith("p:"))[1] || "";
  check(init.slice(2, 16) === "SCRAM-SHA-256\0" && init.slice(20, 28) === "n,,n=,r=", "SASLInitialResponse is SCRAM-SHA-256 with n,,: " + JSON.stringify(init.slice(0, 40)));
  check(fin.startsWith("p:c=biws,r=" + cnonce + "srv,p="), "client-final carries c=biws: " + fin.slice(0, 40));
  check(r.err && /stop here/.test(em(r)), "server error after the proof surfaces: " + em(r));
  check(ticks >= 3, "timers kept firing while 200000 PBKDF2 iterations ran (" + ticks + " ticks): the JS thread was not blocked");
  srv.close();
});

await __kitS.section("rule 18: channelBinding option validation", async () => {
  const boom = new Error("cb boom");
  const base = { host: "127.0.0.1", port: 1, user: "u", connectTimeoutMs: 50 };
  check(syncThrow(() => new PostgreSQL({ ...base, get channelBinding() { throw boom; } })) === boom, "a throwing getter propagates");
  check(syncThrow(() => new PostgreSQL(new Proxy({ ...base, channelBinding: "require" }, { get(t, k) { if (k === "channelBinding") throw boom; return Reflect.get(t, k); } }))) === boom, "a throwing Proxy get trap propagates");
  for (const v of ["REQUIRE", "", "require\0", "prefer ", 1, true, null, { toString() { return "require"; } }]) {
    const e = syncThrow(() => new PostgreSQL({ ...base, channelBinding: v }));
    check(e instanceof TypeError && /channelBinding/.test(e.message), "channelBinding " + JSON.stringify(String(v)) + " is a TypeError: " + e);
  }
  for (const v of ["disable", "prefer", "require", undefined]) {
    const e = syncThrow(() => new PostgreSQL({ ...base, channelBinding: v }));
    check(e === null, "channelBinding " + v + " is accepted: " + e);
  }
});

svc = __kitS.listing ? null : await ensurePostgres({ tls: true });
await __kitS.section("live PostgreSQL over TLS: SCRAM-SHA-256-PLUS tls-server-end-point", async () => {
  if (!svc || !svc.ok || !svc.ca) { print("test_postgres_postgre_sql_scram_plus: SKIP live part: " + (svc ? svc.why || "no CA" : "listing")); return; }
  const base = { ...svc.conn, connectTimeoutMs: 5000 };
  for (const cb of ["require", "prefer", undefined, "disable"]) {
    const c = new PostgreSQL({ ...base, tls: true, ca: svc.ca, channelBinding: cb });
    const r = await settle(c.query("SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()"));
    check(r.ok && r.ok.rows[0].ssl === true, "TLS login with channelBinding " + cb + ": " + em(r));
    c.close();
  }
  let c = new PostgreSQL({ ...base, tls: true, ca: svc.ca, channelBinding: "require", password: "wrong" });
  let r = await settle(c.query("SELECT 1"));
  check(r.err && /password|28P01|authentication/i.test(em(r)), "a wrong password still fails under PLUS: " + em(r));
  c.close();
  c = new PostgreSQL({ ...base, channelBinding: "require" });
  r = await settle(c.query("SELECT 1"));
  check(r.err && /needs tls: true/.test(em(r)), "require over plaintext is refused before the proof: " + em(r));
  c.close();
  c = new PostgreSQL({ ...base, channelBinding: "prefer" });
  r = await settle(c.query("SELECT 1 AS x"));
  check(r.ok && r.ok.rows[0].x === 1, "prefer over plaintext falls back to SCRAM-SHA-256: " + em(r));
  c.close();
  let ws = [];
  for (let i = 0; i < 4; i++) ws.push((async () => { const k = new PostgreSQL({ ...base, tls: true, ca: svc.ca, channelBinding: "require" }); try { return (await k.query("SELECT 1 AS x")).rows[0].x; } finally { k.close(); } })());
  for (let i = 0; i < 10; i++) { std.gc(); await sleep(1); }
  r = await settle(Promise.all(ws));
  check(r.ok && r.ok.join() === "1,1,1,1", "4 concurrent PLUS logins with GC in flight: " + em(r));
});
} catch (e) { check(false, "uncaught: " + (e && e.stack || e)); } finally { await stopStarted(); }
print("test_postgres_postgre_sql_scram_plus: " + (n - fails) + "/" + n + " passed");
if (fails) std.exit(1);
