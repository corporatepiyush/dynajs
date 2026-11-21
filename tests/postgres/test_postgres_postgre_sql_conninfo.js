// flags: --std
// covers: dyna:postgres.PostgreSQL
// Connection conveniences: libpq URIs (postgres:// and postgresql://, userinfo, percent-decoding, bracketed hosts, query parameters overriding userinfo and path),
// PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE, PGPASSFILE or $HOME/.pgpass (read on the I/O pool; close/GC in flight) with the libpq permission check; precedence: explicit option > url > env > .pgpass.
// A fake server records SSLRequest, the startup packet and the cleartext password; the live half logs in to the services.js server by url and by .pgpass.
// Rule 18: hostile toString/getter/Proxy for url and env, a GC inside toString, NUL, 8192/8193-byte urls, secrets never echoed. HOME is a private temp dir.
// TLS failure stops a host list as in libpq 18 fe-connect.c: PQconnectPoll CONNECTION_SSL_STARTUP "SSL handshake failed" -> CONNECTION_FAILED() and an N answer ->
// ENCRYPTION_NEGOTIATION_FAILED() reach error_return once select_next_encryption_method() has no method left (sslmode require/verify-*); pqConnectDBComplete returns 0.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_conninfo");
import * as std from "std";
import * as os from "os";
import { Exec } from "dyna:sys";
import { makeTempDir, removeAll } from "dyna:file";
import { ensurePostgres, stopStarted } from "../lib/services.js";
import { TCPServer } from "dyna:net";
import { PostgreSQL } from "dyna:postgres";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const within = (p, ms = 8000) => Promise.race([p, sleep(ms).then(() => { throw new Error("case timed out after " + ms + " ms"); })]);
async function settle(p) { try { return { ok: await p }; } catch (e) { return { err: e }; } }
const em = (r) => r && r.err ? String(r.err.message || r.err) : "(resolved)";
const str = (u) => { let s = ""; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };
const bin = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };
const i32 = (v) => String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
const be = (s, o) => (s.charCodeAt(o) << 24 | s.charCodeAt(o + 1) << 16 | s.charCodeAt(o + 2) << 8 | s.charCodeAt(o + 3)) >>> 0;
const msg = (t, b) => t + i32(b.length + 4) + b;
function syncThrow(f) { try { const c = f(); if (c && c.close) c.close(); return null; } catch (e) { return e; } }
const PGV = ["PGHOST", "PGPORT", "PGUSER", "PGPASSWORD", "PGDATABASE", "PGPASSFILE"];
const clearEnv = () => { for (const k of PGV) std.unsetenv(k); };
clearEnv();

const svc = __kitS.listing ? null : await ensurePostgres();
const HOME0 = std.getenv("HOME");
const TMP = String(makeTempDir("dj-conninfo-"));
std.setenv("HOME", TMP);
function wfile(name, text, mode) {
  const p = TMP + "/" + name;
  os.remove(p);
  const f = std.open(p, "w"), u = bin(text);
  f.write(u.buffer, 0, u.length); f.close();
  Exec("/bin/chmod", [mode, p]);
  return p;
}

function fake() {
  const seen = [], bufs = new Map(), st = new Map();
  const srv = new TCPServer({ port: 0 });
  srv.start({
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + str(b));
      for (;;) {
        const s = bufs.get(c), rec = st.get(c);
        if (!rec) {
          if (s.length < 8) return;
          const len = be(s, 0);
          if (s.length < len) return;
          bufs.set(c, s.slice(len));
          if (be(s, 4) === 80877103) { seen.push({ ssl: true }); try { c.write(bin("N")); } catch (e) {} continue; }
          const kv = {}, parts = s.slice(8, len).split("\0");
          for (let i = 0; i + 1 < parts.length && parts[i]; i += 2) kv[parts[i]] = parts[i + 1];
          const r = { ssl: false, params: kv, password: undefined };
          seen.push(r); st.set(c, r);
          try { c.write(bin(msg("R", i32(3)))); } catch (e) {}
          continue;
        }
        if (s.length < 5) return;
        const len = be(s, 1);
        if (s.length < len + 1) return;
        const t = s[0], body = s.slice(5, len + 1);
        bufs.set(c, s.slice(len + 1));
        try {
          if (t === "p") { rec.password = body.replace(/\0$/, ""); c.write(bin(msg("R", i32(0)) + msg("K", i32(7) + "abcd") + msg("Z", "I"))); }
          else if (t === "Q") c.write(bin(msg("C", "SELECT 0\0") + msg("Z", "I")));
        } catch (e) {}
      }
    },
    close: (c) => { bufs.delete(c); st.delete(c); },
  });
  srv.seen = seen;
  return srv;
}

const srv = fake(), P = srv.port, H = "127.0.0.1", O = { insecureAuth: true, connectTimeoutMs: 2000 };
async function probe(args, env = {}) {
  clearEnv();
  for (const [k, v] of Object.entries(env)) std.setenv(k, v);
  const before = srv.seen.length;
  let c, err = null;
  try { c = new PostgreSQL(...args); } catch (e) { err = e; }
  clearEnv();
  if (err) return { err, rec: null };
  const q = await settle(within(c.query("SELECT 1"), 3000));
  c.close();
  for (let i = 0; i < 100 && srv.seen.length === before; i++) await sleep(10);
  return { rec: srv.seen[before] || null, q };
}
const PP = (r) => (r.rec && r.rec.params) || {};
const J = (r) => JSON.stringify(r.rec);

await __kitS.section("url: userinfo, host, port, database, parameters, percent-decoding", async () => {
  let r = await probe([`postgresql://us%40er:p%3Aw%2F@${H}:${P}/d%20b?application_name=a%26p`, O]);
  check(PP(r).user === "us@er" && PP(r).database === "d b" && PP(r).application_name === "a&p", "decoded user, database and application_name: " + J(r));
  check(r.rec && r.rec.password === "p:w/" && !r.q.err, "the decoded url password answers the cleartext request: " + J(r) + " " + em(r.q));
  r = await probe([{ ...O, url: `postgres://a:b@${H}:${P}/x?user=c&password=d&dbname=e` }]);
  check(PP(r).user === "c" && PP(r).database === "e" && r.rec.password === "d", "query parameters override userinfo and path; the { url } form: " + J(r));
  r = await probe([`postgres:///shop?host=${H}&port=${P}&user=u&password=pw`, O]);
  check(PP(r).user === "u" && PP(r).database === "shop" && !r.q.err, "host and port as query parameters: " + J(r) + em(r.q));
  r = await probe([`postgresql://u:pw@[${H}]:${P}/d`, O]);
  check(PP(r).user === "u" && !r.q.err, "a bracketed host: " + em(r.q));
  r = await probe([`postgresql://url-user:url-pw@${H}:${P}/urldb?application_name=url-app`, { ...O, user: "opt-user", database: "opt-db", password: "opt-pw", applicationName: "opt-app" }]);
  check(PP(r).user === "opt-user" && PP(r).database === "opt-db" && PP(r).application_name === "opt-app" && r.rec.password === "opt-pw", "explicit options win over the url: " + J(r));
  if (typeof URL === "function") {
    r = await probe([{ ...O, url: new URL(`postgresql://wu@${H}:${P}/wdb`) }]);
    check(PP(r).user === "wu" && PP(r).database === "wdb", "a URL object: " + J(r));
  }
  let calls = 0;
  r = await probe([{ ...O, url: { toString() { calls++; std.gc(); return `postgres://gc@${H}:${P}/g`; } } }]);
  check(PP(r).user === "gc" && calls === 1, "a GC-running toString is called once: " + calls + " " + J(r));
  r = await probe([`postgresql://u:pw@${H}:${P}/d?connect_timeout=1&target_session_attrs=any&sslmode=disable`, O]);
  check(r.rec && !r.rec.ssl && !r.q.err, "connect_timeout, target_session_attrs=any and sslmode=disable are accepted: " + em(r.q));
  r = await probe([`postgresql://u@${H}:${P}/d`]);
  check(PP(r).user === "u" && r.q.err, "a url as the only argument (cleartext still refused without insecureAuth): " + em(r.q));
});

await __kitS.section("url: sslmode and ssl=true start with an SSLRequest; explicit tls wins", async () => {
  for (const q of ["sslmode=require", "sslmode=verify-ca", "sslmode=verify-full", "ssl=true"]) {
    const r = await probe([`postgresql://u@${H}:${P}/d?${q}`, O]);
    check(r.rec && r.rec.ssl === true && r.q.err, q + " sends SSLRequest and never falls back: " + J(r) + " " + em(r.q));
  }
  const r = await probe([`postgresql://u:pw@${H}:${P}/d?sslmode=require`, { ...O, tls: false }]);
  check(r.rec && r.rec.ssl === false && !r.q.err, "explicit tls: false wins over sslmode=require: " + J(r));
});

await __kitS.section("url: malformed input throws synchronously; secrets are never echoed", async () => {
  const bad = [
    ["mysql://h/db", TypeError, /postgres:\/\//], ["postgres://h/%zz", TypeError, /percent/], ["postgres://h/%00", TypeError, /NUL/],
    ["postgres://h/d b", TypeError, /space/], ["postgres://[::1/d", TypeError, /IPv6/], ["postgres://[]/d", TypeError, /IPv6/], ["postgres://[::1]x/d", TypeError, /IPv6/],
    ["postgres://h/d?a=1=2", TypeError, /second/], ["postgres://h/d?flag", TypeError, /without/], ["postgres://h/d?foo=1", TypeError, /'foo' is not supported/],
    ["postgres://h/d?ssl=false", TypeError, /ssl/], ["postgres://h/d?sslmode=prefer", RangeError, /downgrade.*require, verify-full or disable/], ["postgres://h/d?sslmode=allow", RangeError, /downgrade.*require, verify-full or disable/],
    ["postgres://h/d?sslmode=bogus", TypeError, /sslmode/], ["postgres://h:abc/d", RangeError, /port/], ["postgres://h:70000/d", RangeError, /port/],
    ["postgres://h:0/d", RangeError, /port/], ["postgres://a,b,c/d?port=1,2", RangeError, /could not match 2 port numbers to 3 hosts/],
    ["postgres://h/d?target_session_attrs=bogus", RangeError, /invalid target_session_attrs value: "bogus"/], ["postgres://h/d?sslnegotiation=direct", TypeError, /direct/],
    ["postgres://h/d?sslnegotiation=x", TypeError, /sslnegotiation/], ["postgres://h/d?connect_timeout=x", TypeError, /connect_timeout/],
    ["postgres://h/d\0x", TypeError, /NUL/], ["postgres://h/d?connect_timeout=-x", TypeError, /connect_timeout/],
  ];
  for (const [u, T, re] of bad) {
    const e = syncThrow(() => new PostgreSQL(u, { connectTimeoutMs: 200 }));
    check(e instanceof T && re.test(e.message), JSON.stringify(u) + " throws " + T.name + " " + re + ": " + e);
  }
  const base = "postgres://127.0.0.1:1/";
  check(!syncThrow(() => new PostgreSQL(base + "x".repeat(8192 - base.length), { connectTimeoutMs: 200 })), "an 8192-byte url is accepted");
  const big = syncThrow(() => new PostgreSQL(base + "x".repeat(8193 - base.length)));
  check(big instanceof RangeError && /8192/.test(big.message), "an 8193-byte url is a RangeError: " + big);
  check(!syncThrow(() => new PostgreSQL(base + "d?sslmode=require&sslnegotiation=direct&sslrootcert=system", { connectTimeoutMs: 200 })), "sslnegotiation=direct with sslmode=require is accepted");
  const others = [
    ["a url in both places", () => new PostgreSQL(base + "d", { url: base + "e" }), /twice/],
    ["url 42", () => new PostgreSQL({ url: 42 }), /string or URL/],
    ["a number as the first argument", () => new PostgreSQL(42), /options object/],
    ["a number after the url", () => new PostgreSQL(base + "d", 5), /options object/],
    ["url toString throws", () => new PostgreSQL({ url: { toString() { throw new Error("ts boom"); } } }), /ts boom/],
    ["url getter throws", () => new PostgreSQL({ port: 1, get url() { throw new Error("getter boom"); } }), /getter boom/],
    ["env getter throws", () => new PostgreSQL({ port: 1, get env() { throw new Error("env boom"); } }), /env boom/],
    ["Proxy get url throws", () => new PostgreSQL(new Proxy({ port: 1 }, { get: (t, k) => { if (k === "url") throw new Error("proxy boom"); return t[k]; } })), /proxy boom/],
  ];
  for (const [name, f, re] of others) { const e = syncThrow(f); check(e && re.test(String(e.message)), name + ": " + e); }
  for (const u of ["postgres://u:s3cr3tPW@h/d?foo=1", "postgres://u:s3cr3tPW@h:abc/d", "postgres://u:s3cr3tPW@[::1/d", "postgres://u:s3cr3tPW@h/d%zz", "postgres://u:s3cr3tPW@h/d?sslmode=prefer"]) {
    const e = syncThrow(() => new PostgreSQL(u));
    check(e && !String(e.message).includes("s3cr3t") && !String(e.stack || "").includes("s3cr3t"), "no secret in the error for " + u.replace("s3cr3tPW", "***") + ": " + (e && e.message));
  }
});

await __kitS.section("env: PGHOST, PGPORT, PGUSER, PGPASSWORD, PGDATABASE", async () => {
  let r = await probe([O], { PGHOST: H, PGPORT: String(P), PGUSER: "envu", PGPASSWORD: "envpw", PGDATABASE: "envdb" });
  check(PP(r).user === "envu" && PP(r).database === "envdb" && r.rec.password === "envpw" && !r.q.err, "the environment fills unset options: " + J(r) + em(r.q));
  r = await probe([], { PGHOST: H, PGPORT: String(P), PGUSER: "bare" });
  check(PP(r).user === "bare", "new PostgreSQL() with no arguments reads the environment: " + J(r));
  r = await probe([`postgresql://urlu:urlpw@${H}/urldb`, O], { PGPORT: String(P), PGUSER: "envu", PGPASSWORD: "envpw", PGDATABASE: "envdb" });
  check(PP(r).user === "urlu" && PP(r).database === "urldb" && r.rec.password === "urlpw", "the url wins over the environment; PGPORT fills the missing port: " + J(r));
  r = await probe([{ ...O, host: H, port: P, user: "optu" }], { PGUSER: "envu", PGDATABASE: "envdb" });
  check(PP(r).user === "optu" && PP(r).database === "envdb", "options win over the environment, which fills the rest: " + J(r));
  r = await probe([{ ...O, host: H, port: P, env: false }], { PGUSER: "envu", PGPASSWORD: "envpw" });
  check(PP(r).user === "postgres" && r.rec.password === undefined, "env: false ignores PG* variables: " + J(r));
  r = await probe([O], { PGPORT: "abc" });
  check(r.err instanceof RangeError && /PGPORT/.test(r.err.message), "a bad PGPORT is a RangeError: " + r.err);
  r = await probe([{ ...O, user: "alice", password: "pw" }], { PGHOST: H + "," + H, PGPORT: "1," + P });
  check(r.rec && r.q && !r.q.err, "a PGHOST/PGPORT list tries each host in order: " + J(r) + em(r.q));
  check(!syncThrow(() => { std.setenv("PGPORT", "abc"); try { return new PostgreSQL({ port: 1, env: false, connectTimeoutMs: 200 }); } finally { clearEnv(); } }), "env: false never reads a bad PGPORT");
});

await __kitS.section(".pgpass: PGPASSFILE and $HOME/.pgpass, escapes, wildcards, permissions, precedence", async () => {
  const url = `postgresql://alice@${H}:${P}/shop`;
  const pw = async (file, mode, args = [url, O], env = {}) => (await probe(args, { PGPASSFILE: wfile("pgpass", file, mode), ...env })).rec;
  let rec = await pw(`# comment\nother:*:*:*:no\n${H}:${P}:shop:alice:s\\:e\\\\c\n`, "600");
  check(rec && rec.password === "s:e\\c", "an escaped line answers: " + JSON.stringify(rec));
  rec = await pw("*:*:*:alice:wild\n", "600");
  check(rec && rec.password === "wild", "wildcards: " + JSON.stringify(rec));
  rec = await pw(`*:*:*:*:first\n${H}:${P}:shop:alice:second\n`, "600");
  check(rec && rec.password === "first", "the first match wins: " + JSON.stringify(rec));
  rec = await pw(`${H}:${P}:shop:alice:crlf\r\n`, "600");
  check(rec && rec.password === "crlf", "CRLF line ends: " + JSON.stringify(rec));
  rec = await pw(`${H}:${P}:shop:alice:ro\n`, "400");
  check(rec && rec.password === "ro", "mode 0400 is read: " + JSON.stringify(rec));
  for (const m of ["644", "640", "604", "660", "700"]) {
    rec = await pw(`${H}:${P}:shop:alice:x\n`, m);
    const want = m === "700" ? "x" : undefined;
    check(rec && rec.password === want, "mode " + m + (want ? " is read: " : " is ignored: ") + JSON.stringify(rec));
  }
  rec = await pw(`${H}:${P}:shop:alice:x\0\n`, "600");
  check(rec && rec.password === undefined, "a NUL byte makes the file ignored: " + JSON.stringify(rec));
  rec = await pw("#" + "x".repeat(1100000) + `\n${H}:${P}:shop:alice:big\n`, "600");
  check(rec && rec.password === undefined, "a file over 1 MiB is ignored: " + JSON.stringify(rec));
  rec = await pw(`${H}:${P}:shop:alice:filepw\n`, "600", [url, O], { PGPASSWORD: "envpw" });
  check(rec && rec.password === "envpw", "PGPASSWORD wins over .pgpass: " + JSON.stringify(rec));
  rec = await pw(`${H}:${P}:shop:alice:filepw\n`, "600", [`postgresql://alice:urlpw@${H}:${P}/shop`, O]);
  check(rec && rec.password === "urlpw", "a url password wins over .pgpass: " + JSON.stringify(rec));
  rec = await pw(`${H}:${P}:shop:alice:filepw\n`, "600", [url, { ...O, env: false }]);
  check(rec && rec.password === undefined, "env: false skips .pgpass: " + JSON.stringify(rec));
  rec = await pw(`${H}:${P}:alice:alice:dbuser\n`, "600", [`postgresql://alice@${H}:${P}`, O]);
  check(rec && rec.password === "dbuser", "the database defaults to the user for matching: " + JSON.stringify(rec));
  rec = await pw(`localhost:${P}:shop:alice:lh\n`, "600", [{ ...O, port: P, user: "alice", database: "shop" }]);
  check(rec && rec.password === "lh", "no host matches localhost: " + JSON.stringify(rec));
  rec = await pw(`${H}:5432:shop:alice:wrongport\n`, "600");
  check(rec && rec.password === undefined, "the port must match: " + JSON.stringify(rec));
  {
    const d = TMP + "/adir";
    os.mkdir(d, 0o700);
    const r = await probe([url, O], { PGPASSFILE: d });
    check(r.rec && r.rec.password === undefined, "a directory is ignored: " + J(r));
    os.remove(d);
  }
  {
    const f = wfile(".pgpass", `${H}:${P}:shop:alice:homepw\n`, "600");
    const r = await probe([url, O]);
    check(r.rec && r.rec.password === "homepw", "$HOME/.pgpass when PGPASSFILE is unset: " + J(r));
    os.remove(f);
  }
  os.remove(TMP + "/pgpass");
});

await __kitS.section(".pgpass on the I/O pool: close and GC while the read is in flight, a 1 MB file, connect errors reject", async () => {
  const dead = "postgresql://alice@127.0.0.1:1/shop", D = { connectTimeoutMs: 2000 };
  const bigf = wfile("pgpass-big", "#" + "x".repeat(1000000) + `\n${H}:${P}:shop:alice:late\n127.0.0.1:1:shop:alice:late\n`, "600");
  std.setenv("PGPASSFILE", bigf);
  try {
    let closed = 0, other = "";
    for (let i = 0; i < 40; i++) {
      const c = new PostgreSQL(dead, D);
      const q = settle(c.query("select 1"));
      c.close();
      const r = await within(q);
      if (r.err && /client closed/.test(r.err.message)) closed++; else other = em(r);
    }
    check(closed === 40, "close() while the read is in flight rejects with client closed: " + closed + "/40 " + other);
    for (let i = 0; i < 40; i++) new PostgreSQL(dead, D);
    std.gc();
    await sleep(150);
    std.gc();
    const e = await within(settle((async () => { const c = new PostgreSQL(dead, D); try { return await c.query("select 1"); } finally { c.close(); } })()));
    check(e.err && /connect/.test(e.err.message) && !/late/.test(e.err.message), "after the read a refused connect rejects the query, no secret in it: " + em(e));
  } finally { std.unsetenv("PGPASSFILE"); }
  const r = await probe([`postgresql://alice@${H}:${P}/shop`, O], { PGPASSFILE: bigf });
  check(r.rec && r.rec.password === "late" && !r.q.err, "a 1 MB .pgpass read on the I/O pool answers: " + J(r) + em(r.q));
  os.remove(bigf);
});

function rawSrv(answer) {
  const s = new TCPServer({ port: 0 });
  s.hits = 0;
  s.start({ data: (c) => { if (s.hits++ === 0) { try { answer(c); } catch (e) {} } } });
  return s;
}
const errMsg = (code, m) => bin(msg("E", "SFATAL\0C" + code + "\0M" + m + "\0\0"));

await __kitS.section("multi-host (libpq 18 34.1.1.3): lists, next host on refused, 57P03 and per-host timeout, never after an auth error, per-host .pgpass, error text, 1024-host cap", async () => {
  let r = await probe([`postgresql://alice:pw@${H}:1,${H}:${P}/shop`, O]);
  check(r.rec && r.rec.password === "pw" && !r.q.err, "a refused first host falls through to the second: " + J(r) + em(r.q));
  r = await probe([`postgresql:///shop?host=${H},${H}&port=${P}&user=alice&password=pw`, O]);
  check(r.q && !r.q.err && PP(r).user === "alice", "one port serves every host: " + em(r.q));
  r = await probe(["postgresql:///shop?host=a,b,c&port=1,2&user=u", O]);
  check(r.err instanceof RangeError && /could not match 2 port numbers to 3 hosts/.test(r.err.message), "2 ports for 3 hosts is a RangeError: " + r.err);
  r = await probe([`postgresql:///shop?host=a,b&hostaddr=${H}&user=u`, O]);
  check(r.err instanceof RangeError && /could not match 2 host names to 1 hostaddr values/.test(r.err.message), "2 host names for 1 hostaddr is a RangeError: " + r.err);
  r = await probe([`postgresql:///shop?host=a,b&hostaddr=${H},${H}&port=1,${P}&user=alice`, O], { PGPASSFILE: wfile("pgpass-mh", "a:*:*:*:pa\nb:*:*:*:pb\n", "600") });
  check(r.rec && r.rec.password === "pb" && !r.q.err, "each host's .pgpass match uses that host's name (b, after a refused): " + J(r) + em(r.q));
  r = await probe([{ ...O, host: "x,y", hostaddr: `${H},${H}`, port: P, user: "alice", password: "pw", database: "shop" }]);
  check(r.q && !r.q.err, "the hostaddr option pairs with host: " + em(r.q));
  r = await probe([`postgresql:///shop?hostaddr=not-an-ip,${H}&port=${P}&user=alice&password=pw`, O]);
  check(r.q && !r.q.err, "a non-numeric hostaddr fails that host only: " + em(r.q));
  {
    const c = new PostgreSQL(`postgresql://alice:sekrit@${H}:1,${H}:2/shop`, { ...O, env: false });
    const e = await settle(within(c.query("select 1"))); c.close();
    const m = em(e);
    check(/^PostgreSQL: connection to server at "127\.0\.0\.1", port 1 failed: [^\n]+\nconnection to server at "127\.0\.0\.1", port 2 failed: /.test(m) && !/sekrit/.test(m), "every host failed: libpq-style reasons joined by newlines, no password: " + JSON.stringify(m));
  }
  const s57 = rawSrv((c) => c.write(errMsg("57P03", "the database system is starting up")));
  r = await probe([`postgresql://alice:pw@${H}:${s57.port},${H}:${P}/shop`, O]);
  check(s57.hits > 0 && r.rec && r.q && !r.q.err, "57P03 cannot_connect_now moves to the next host: " + em(r.q));
  const s28 = rawSrv((c) => c.write(errMsg("28P01", "password authentication failed for user")));
  const n0 = srv.seen.length;
  r = await probe([`postgresql://alice:pw@${H}:${s28.port},${H}:${P}/shop`, O]);
  check(s28.hits > 0 && r.q && r.q.err && /password authentication failed/.test(em(r.q)) && srv.seen.length === n0, "an authentication error stops: the next host never sees the password: " + em(r.q));
  const sh = rawSrv(() => {});
  const t0 = Date.now();
  r = await probe([`postgresql://alice:pw@${H}:${sh.port},${H}:${P}/shop?connect_timeout=2`, { insecureAuth: true }]);
  const dt = Date.now() - t0;
  // wall-clock: connect_timeout is a per-host deadline; the hung host must be dropped after about 2 s while the whole connect still succeeds
  check(r.q && !r.q.err && dt >= 1500, "connect_timeout=2 applies per host: the hung host is dropped, the next answers after " + dt + " ms: " + em(r.q));
  check(!syncThrow(() => new PostgreSQL({ host: Array(1024).fill("h").join(","), port: 1, env: false })), "1024 hosts construct");
  const e2 = syncThrow(() => new PostgreSQL({ host: Array(1025).fill("h").join(","), port: 1, env: false }));
  check(e2 instanceof RangeError && /at most 1024 hosts/.test(e2.message), "1025 hosts is a RangeError at construction: " + e2);
  const e3 = syncThrow(() => new PostgreSQL({ get hostaddr() { throw new Error("boom-ha"); }, port: 1, env: false }));
  check(e3 && /boom-ha/.test(e3.message), "a throwing hostaddr getter propagates: " + e3);
  const e4 = syncThrow(() => new PostgreSQL({ hostaddr: "127.0.0.1\0x", port: 1, env: false }));
  check(e4 instanceof TypeError, "a NUL byte in hostaddr is a TypeError: " + e4);
  for (const s of [s57, s28, sh]) s.close();
});

await __kitS.section("TLS failure stops the host list (libpq 18 fe-connect.c error_return): a failed handshake, an N answer", async () => {
  const sS = rawSrv((c) => { c.write(bin("S")); setTimeout(() => { try { c.write(bin("\x15\x03\x03\x00\x02\x02\x28")); } catch (e) {} }, 50); });
  const sN = rawSrv((c) => c.write(bin("N")));
  for (const [s, re, name] of [[sS, /TLS handshake failed/, "a failed TLS handshake"], [sN, /does not accept TLS/, "an N answer to SSLRequest"]]) {
    const n0 = srv.seen.length;
    const c = new PostgreSQL(`postgresql://alice:pw@${H}:${s.port},${H}:${P}/shop`, { tls: true, connectTimeoutMs: 3000, env: false });
    const r = await settle(within(c.query("select 1"))); c.close();
    await sleep(100);
    check(r.err && re.test(em(r)) && srv.seen.length === n0 && !/pw/.test(em(r)), name + " stops: the next host is never contacted: " + em(r));
  }
  sS.close(); sN.close();
});

await __kitS.section("host/hostaddr/port list differential: libpq 18 pqConnectOptions2 + PQconnectPoll CONNECTION_NEEDED + pqParseIntParam; a 5th column is a deliberate dyna difference", async () => {
  const A = "127.0.0.1", S = "/nonexistent-dj-sock", at = (h, p) => `at "${h}", port ${p}`, so = (p) => `on socket "${S}/.s.PGSQL.${p}"`;
  const esc = (s) => s.replace(/[.*+?^${}()|[\]\\\/]/g, "\\$&");
  const T = [
    [A, null, "1", [at(A, 1)]],
    [`${A},${A}`, null, "1", [at(A, 1), at(A, 1)]],
    [`${A},${A}`, null, "1,2", [at(A, 1), at(A, 2)]],
    [`${A},${A},${A}`, null, "1,2", /could not match 2 port numbers to 3 hosts/],
    [A, null, "1,2", /could not match 2 port numbers to 1 hosts/],
    [`${A},${A}`, null, "1,2,3", /could not match 3 port numbers to 2 hosts/],
    [`${S},${S}`, null, null, [so(5432), so(5432)]],
    [`${S},${A}`, null, "7,1", [so(7), at(A, 1)]],
    [`${S},${S}`, null, ",1", [so(5432), so(1)]],
    [`${S},${S}`, null, ",", [so(5432), so(5432)]],
    [null, A, "1", [at(A, 1)]],
    [null, `${A},${A}`, "1,2", [at(A, 1), at(A, 2)]],
    ["x,y", `${A},${A}`, "1,2", [at(A, 1), at(A, 2)]],
    ["x", `${A},${A}`, "1", /could not match 1 host names to 2 hostaddr values/],
    ["x,y", A, "1", /could not match 2 host names to 1 hostaddr values/],
    [`x,${S}`, `${A},`, "1,7", [at(A, 1), so(7)]],
    [null, "::1", "1", [at("::1", 1)]],
    [A, "", "1", [at(A, 1)]],
    [A, null, "+1", ["libpq: port +1"], [at(A, 1)]],
    [A, null, " 1 ", ["libpq: port  1 "], [at(A, 1)]],
    [`${A},${A}`, null, " 1, 2", ["libpq: ports  1 and  2"], [at(A, 1), at(A, 2)]],
    [A, null, "0", ["libpq: invalid port number at that host's turn, next host"], /invalid port number: "0"/],
    [A, null, "65536", ["libpq: invalid port number at that host's turn"], /invalid port number: "65536"/],
    [A, null, "-1", ["libpq: invalid port number at that host's turn"], /invalid port number: "-1"/],
    [A, null, "1x", ["libpq: invalid integer value at that host's turn, stop"], /invalid integer value "1x" for connection option "port"/],
    [A, null, "99999999999", ["libpq: invalid integer value (overflow)"], /invalid integer value "99999999999"/],
    [`${A},${A}`, null, "1,abc", ["libpq: host 1 tried, then invalid integer value, stop"], /invalid integer value "abc"/],
    [`${A},,${A}`, null, "1,2,3", ["libpq: empty host = default socket dir"], [at(A, 1), at(A, 2), at(A, 3)]],
    [",,", null, "1", ["libpq: empty hosts = default socket dir"], [at(A, 1), at(A, 1), at(A, 1)]],
    [null, "nope", "1", ["libpq: could not parse network address without the server prefix"], [/at "nope", port 1(?= failed: could not parse network address "nope")/]],
    [Array(1025).fill(A).join(","), null, "1", ["libpq: no cap"], /at most 1024 hosts/],
  ];
  for (const [h, a, p, want, dyna] of T) {
    clearEnv();
    if (h !== null) std.setenv("PGHOST", h);
    if (a !== null) std.setenv("PGHOSTADDR", a);
    if (p !== null) std.setenv("PGPORT", p);
    let c = null, err = null;
    try { c = new PostgreSQL({ user: "u", password: "pw", connectTimeoutMs: 2000 }); } catch (e) { err = e; } finally { clearEnv(); std.unsetenv("PGHOSTADDR"); }
    const exp = dyna !== undefined ? dyna : want;
    const tag = JSON.stringify([h && h.length > 40 ? h.length + " chars" : h, a, p]) + (dyna !== undefined ? " [dyna differs: " + want[0] + "]" : "");
    if (exp instanceof RegExp) { if (c) c.close(); check(err instanceof RangeError && exp.test(err.message), tag + " throws RangeError " + exp + ": " + err); continue; }
    if (err) { check(false, tag + " threw " + err); continue; }
    const r = await settle(within((async () => c.query("select 1"))())); c.close();
    const re = new RegExp("^PostgreSQL: " + exp.map((d) => "connection to server " + (d instanceof RegExp ? d.source : esc(d)) + " failed: [^\\n]*").join("\\n") + "$");
    check(r.err && re.test(em(r)), tag + " tries " + JSON.stringify(exp.map(String)) + ": " + JSON.stringify(em(r)));
  }
});

await __kitS.section("a failed host list: every host's reason reaches later queries, in full; a client that connected forgets earlier hosts' failures", async () => {
  {
    const hs = Array.from({ length: 40 }, (_, i) => "/nonexistent-dj-sock/h" + i);
    const c = new PostgreSQL({ host: hs.join(","), user: "u", database: "d", env: false });
    const e = await settle(within((async () => c.query("select 1"))()));
    c.close();
    const m = em(e), lines = m.split("\n");
    check(lines.length === 40 && hs.every((h, i) => lines[i].startsWith((i ? "" : "PostgreSQL: ") + `connection to server on socket "${h}/.s.PGSQL.5432" failed: `)) && e.err.code === "CONNECTION",
          "40 failed socket hosts: the dead client's query error lists all 40 reasons untruncated, code CONNECTION: " + lines.length + " lines, " + m.length + " chars, code " + (e.err && e.err.code));
  }
  {
    let dn = 0;
    const drop = new TCPServer({ port: 0 });
    drop.start({ data: (c) => { try { if (dn++ === 0) c.write(bin(msg("R", i32(0)) + msg("K", i32(7) + "abcd") + msg("Z", "I"))); else { c.write(bin(msg("C", "SELECT 0\0") + msg("Z", "I"))); c.close(); } } catch (e) {} } });
    const c = new PostgreSQL(`postgresql://alice@${H}:1,${H}:${drop.port}/shop`, { ...O, env: false });
    const q1 = await settle(within(c.query("select 1")));
    for (let i = 0; i < 200 && dn < 2; i++) await sleep(10);
    await sleep(100);
    const q2 = await settle(within((async () => c.query("select 1"))()));
    c.close(); drop.close();
    check(!q1.err && q2.err && !/port 1 failed/.test(em(q2)), "host 1 refused, host 2 connected then dropped: the later error is not host 1's stale reason: " + em(q1) + " / " + em(q2));
  }
});

function roleSrv(o = {}) {
  const s = new TCPServer({ port: 0 }), bufs = new Map(), started = new Set();
  s.conns = 0; s.q = []; s.bye = 0; s.checks = [];
  const one = (v) => msg("T", "\0\x01x\0" + i32(0) + "\0\0" + i32(25) + "\xff\xff" + i32(-1) + "\0\0") + (v === undefined ? "" : msg("D", "\0\x01" + (v === null ? i32(-1) : i32(v.length) + v))) + msg("C", "SELECT 1\0") + msg("Z", "I");
  const ans = (v) => v === "error" ? msg("E", "SERROR\0C42501\0Mpermission denied\0\0") + msg("Z", "I") : v === "two" ? msg("T", "\0\x01x\0" + i32(0) + "\0\0" + i32(25) + "\xff\xff" + i32(-1) + "\0\0") + msg("D", "\0\x01" + i32(3) + "off") + msg("D", "\0\x01" + i32(3) + "off") + msg("C", "SELECT 2\0") + msg("Z", "I") : one(v);
  s.start({
    connect: () => { s.conns++; },
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + str(b));
      for (;;) {
        const d = bufs.get(c);
        if (!started.has(c)) {
          if (d.length < 8 || d.length < be(d, 0)) return;
          bufs.set(c, d.slice(be(d, 0))); started.add(c);
          let out = msg("R", i32(0));
          for (const [k, v] of Object.entries(o.params || {})) out += msg("S", k + "\0" + v + "\0");
          try { c.write(bin(out + msg("K", i32(7) + "abcd") + msg("Z", "I"))); } catch (e) {}
          continue;
        }
        if (d.length < 5 || d.length < be(d, 1) + 1) return;
        const t = d[0], body = d.slice(5, be(d, 1) + 1);
        bufs.set(c, d.slice(be(d, 1) + 1));
        try {
          if (t === "X") s.bye++;
          else if (t === "Q" && /transaction_read_only/.test(body)) { s.checks.push("show"); c.write(bin(ans(o.show))); }
          else if (t === "Q" && /pg_is_in_recovery/.test(body)) { s.checks.push("recovery"); c.write(bin(ans(o.rec))); }
          else if (t === "Q") { s.q.push(body.replace(/\0$/, "")); c.write(bin(one("1"))); }
        } catch (e) {}
      }
    },
    close: (c) => { bufs.delete(c); started.delete(c); },
  });
  return s;
}
const PRIM = { in_hot_standby: "off", default_transaction_read_only: "off", server_version: "18.1" };
const STBY = { in_hot_standby: "on", default_transaction_read_only: "off", server_version: "18.1" };
const ROPRIM = { in_hot_standby: "off", default_transaction_read_only: "on", server_version: "18.1" };
async function tsa(servers, attrs, extra = {}, url = "") {
  const c = new PostgreSQL(`postgresql://u@${servers.map((s) => H + ":" + s.port).join(",")}/d?target_session_attrs=${attrs}${url}`, { env: false, ...extra });
  const r = await settle(within((async () => c.query("select 1"))(), 9000));
  c.close();
  await sleep(30);
  return r;
}
const used = (s) => s.q.includes("select 1");

await __kitS.section("target_session_attrs (libpq 18 PQconnectPoll CONNECTION_CHECK_TARGET/CHECK_WRITABLE/CHECK_STANDBY) and load_balance_hosts", async () => {
  {
    const a = roleSrv({ params: STBY }), b = roleSrv({ params: PRIM });
    const r = await tsa([a, b], "read-write");
    check(!r.err && !used(a) && used(b) && a.bye === 1 && a.checks.length === 0 && b.checks.length === 0, "read-write skips a hot standby (Terminate sent, no query there, no SHOW when both ParameterStatus are known), uses the primary: " + em(r) + " a.bye=" + a.bye + " a.q=" + a.q);
    a.close(); b.close();
  }
  {
    const a = roleSrv({ params: ROPRIM }), b = roleSrv({ params: PRIM });
    const r = await tsa([a, b], "read-write");
    check(!r.err && !used(a) && used(b), "read-write: default_transaction_read_only=on alone is read-only: " + em(r));
    a.close(); b.close();
  }
  {
    const a = roleSrv({ params: PRIM }), b = roleSrv({ params: ROPRIM });
    const r = await tsa([a, b], "read-only");
    check(!r.err && !used(a) && used(b) && a.bye === 1, "read-only skips a writable primary, takes a read-only one: " + em(r));
    a.close(); b.close();
  }
  {
    const a = roleSrv({ params: ROPRIM }), b = roleSrv({ params: STBY });
    let r = await tsa([a, b], "standby");
    check(!r.err && !used(a) && used(b), "standby skips a primary even when read-only, takes in_hot_standby=on: " + em(r));
    const c2 = roleSrv({ params: STBY }), d2 = roleSrv({ params: ROPRIM });
    r = await tsa([c2, d2], "primary");
    check(!r.err && !used(c2) && used(d2), "primary skips a hot standby, takes a read-only primary: " + em(r));
    for (const s of [a, b, c2, d2]) s.close();
  }
  {
    const a = roleSrv({ params: { server_version: "13.4" }, show: "on" }), b = roleSrv({ params: { server_version: "13.4" }, show: "off" });
    const r = await tsa([a, b], "read-write");
    check(!r.err && a.checks.join() === "show" && b.checks.join() === "show" && !used(a) && used(b), "server without in_hot_standby: read-write runs SHOW transaction_read_only on each host (on = skip): " + em(r) + " " + a.checks + "/" + b.checks);
    a.close(); b.close();
  }
  {
    const a = roleSrv({ params: { server_version: "13.4" }, rec: "t" }), b = roleSrv({ params: { server_version: "13.4" }, rec: "f" });
    const r = await tsa([a, b], "primary");
    check(!r.err && a.checks.join() === "recovery" && !used(a) && used(b), "server without in_hot_standby: primary runs pg_is_in_recovery() (t = skip): " + em(r));
    a.close(); b.close();
  }
  {
    const a = roleSrv({ params: { server_version: "8.4.22" }, rec: "t" });
    const r = await tsa([a], "primary");
    check(!r.err && a.checks.length === 0 && used(a), "a server older than 9.0 counts as primary without a query (libpq sversion < 90000): " + em(r));
    const b = roleSrv({ params: { server_version: "8.4.22" } });
    const r2 = await tsa([b], "standby");
    check(r2.err && /server is not in hot standby mode/.test(em(r2)), "a server older than 9.0 is never a standby: " + em(r2));
    a.close(); b.close();
  }
  {
    const a = roleSrv({ params: { server_version: "13.4" }, show: "error" }), b = roleSrv({ params: { server_version: "13.4" }, show: "two" }), c3 = roleSrv({ params: { server_version: "13.4" }, show: "off" });
    const r = await tsa([a, b, c3], "read-write");
    check(!r.err && used(c3) && !used(a) && !used(b), "a failed or 2-row SHOW moves to the next host: " + em(r));
    const z = roleSrv({ params: { server_version: "13.4" }, show: "error" });
    const r2 = await tsa([z], "read-write");
    check(r2.err && /^PostgreSQL: connection to server at "127\.0\.0\.1", port \d+ failed: "SHOW transaction_read_only" failed$/.test(em(r2)), "the failed check is reported libpq-style: " + em(r2));
    for (const s of [a, b, c3, z]) s.close();
  }
  {
    const a = roleSrv({ params: STBY }), b = roleSrv({ params: STBY });
    const r = await tsa([a, b], "read-write");
    const m = em(r);
    check(r.err && m.split("\n").length === 2 && m.split("\n").every((l) => /failed: session is read-only$/.test(l)) && !used(a) && !used(b), "no writable host: every host's reason, no query anywhere: " + m);
    a.close(); b.close();
  }
  {
    const a = roleSrv({ params: PRIM }), b = roleSrv({ params: PRIM });
    const r = await tsa([a, b], "prefer-standby");
    check(!r.err && used(a) && !used(b) && a.conns === 2 && b.conns === 1, "prefer-standby with no standby: a second pass takes the first host as any: " + em(r) + " conns " + a.conns + "/" + b.conns);
    const c4 = roleSrv({ params: PRIM }), d4 = roleSrv({ params: STBY });
    const r2 = await tsa([c4, d4], "prefer-standby");
    check(!r2.err && used(d4) && !used(c4) && c4.conns === 1, "prefer-standby picks the standby on the first pass: " + em(r2));
    const r3 = await settle(within((async () => { const c = new PostgreSQL(`postgresql://u@${H}:1,${H}:2/d?target_session_attrs=prefer-standby`, { env: false }); try { return await c.query("select 1"); } finally { c.close(); } })()));
    check(r3.err && em(r3).split("\n").length === 4, "prefer-standby with every host refused tries the list twice, as libpq: " + em(r3));
    for (const s of [a, b, c4, d4]) s.close();
  }
  {
    const a = roleSrv({ params: STBY }), b = roleSrv({ params: PRIM });
    let r = await tsa([a, b], "any", { targetSessionAttrs: "read-write" });
    check(!r.err && used(b) && !used(a), "the targetSessionAttrs option wins over the url: " + em(r));
    clearEnv(); std.setenv("PGTARGETSESSIONATTRS", "read-write");
    const c = new PostgreSQL(`postgresql://u@${H}:${a.port},${H}:${b.port}/d`);
    clearEnv(); std.unsetenv("PGTARGETSESSIONATTRS");
    r = await settle(within(c.query("select 1"))); c.close();
    check(!r.err && b.q.length === 2 && a.q.length === 0, "PGTARGETSESSIONATTRS applies: " + em(r));
    a.close(); b.close();
  }
  {
    const bad = [
      [() => new PostgreSQL("postgres://h/d?target_session_attrs=Read-Write", { env: false }), RangeError, /invalid target_session_attrs value: "Read-Write"/],
      [() => new PostgreSQL("postgres://h/d?load_balance_hosts=yes", { env: false }), RangeError, /invalid load_balance_hosts value: "yes"/],
      [() => new PostgreSQL({ host: H, targetSessionAttrs: "primary\0x", env: false }), TypeError, /NUL/],
      [() => new PostgreSQL({ host: H, loadBalanceHosts: { toString() { throw new Error("lb-boom"); } }, env: false }), Error, /lb-boom/],
      [() => new PostgreSQL({ host: H, get targetSessionAttrs() { throw new Error("tsa-getter"); }, env: false }), Error, /tsa-getter/],
      [() => new PostgreSQL({ host: H, targetSessionAttrs: "x".repeat(100000), env: false }), RangeError, /invalid target_session_attrs value: "x{64}"$/],
    ];
    for (const [f, cls, re] of bad) { const e = syncThrow(f); check(e instanceof cls && re.test(e.message), "rejected at construction: " + re + " got " + e); }
  }
  {
    const ss = [roleSrv({ params: PRIM }), roleSrv({ params: PRIM }), roleSrv({ params: PRIM }), roleSrv({ params: PRIM })];
    const list = ss.map((s) => H + ":" + s.port).join(",");
    const firsts = (n, lb) => Promise.all(Array.from({ length: n }, async () => { const c = new PostgreSQL(`postgresql://u@${list}/d?load_balance_hosts=${lb}`, { env: false }); const r = await settle(within(c.query("select 1"))); c.close(); return r.err ? -1 : 0; }));
    await firsts(40, "random");
    const hit = ss.map((s) => s.q.length);
    check(hit.every((h) => h >= 1) && hit.reduce((x, y) => x + y, 0) === 40, "load_balance_hosts=random spreads 40 clients over all 4 hosts: " + hit);
    for (const s of ss) s.q.length = 0;
    await firsts(10, "disable");
    check(ss[0].q.length === 10, "load_balance_hosts=disable always takes the first host: " + ss.map((s) => s.q.length));
    for (const s of ss) s.close();
  }
});

await __kitS.section("live: url and .pgpass against the server", async () => {
  if (!svc || !svc.ok) { print("test_postgres_postgre_sql_conninfo: SKIP live part: " + (svc ? svc.why : "listing")); return; }
  const { host, port, user, password, database } = svc.conn, enc = encodeURIComponent;
  const c = new PostgreSQL(`postgresql://${enc(user)}:${enc(password)}@${host}:${port}/${enc(database)}?application_name=dj%20conninfo`, { connectTimeoutMs: 5000 });
  try {
    const r = await settle(within(c.query("SELECT current_user AS u, current_setting('application_name') AS a")));
    check(!r.err && r.ok.rows[0].u === user && r.ok.rows[0].a === "dj conninfo", "live: url login and application_name: " + em(r));
  } finally { c.close(); }
  const esc = (s) => s.replace(/\\/g, "\\\\").replace(/:/g, "\\:");
  const line = `${host}:${port}:${esc(database)}:${esc(user)}:${esc(password)}\n`;
  for (const [mode, want] of [["600", true], ["644", false]]) {
    const f = wfile("live-pgpass", line, mode);
    std.setenv("PGPASSFILE", f);
    let c2;
    try { c2 = new PostgreSQL(`postgresql://${enc(user)}@${host}:${port}/${enc(database)}`, { connectTimeoutMs: 5000 }); } finally { clearEnv(); }
    const r = await settle(within(c2.query("SELECT 1 AS one")));
    c2.close();
    os.remove(f);
    check(want ? !r.err && r.ok.rows[0].one === 1 : !!r.err, "live: .pgpass mode " + mode + (want ? " logs in: " : " is ignored: ") + em(r));
  }
});

srv.close();
await removeAll(TMP);
if (HOME0 !== undefined && HOME0 !== null) std.setenv("HOME", HOME0);
stopStarted();
print(fails ? "test_postgres_postgre_sql_conninfo: " + fails + " of " + n + " checks FAILED" : "test_postgres_postgre_sql_conninfo: all " + n + " checks passed");
if (fails) std.exit(1);
