// covers: dyna:postgres.PostgreSQL
// Split from tests/net/test_net_fragment.js (one module per suite).
import { TCPServer } from "dyna:net";
import { PostgreSQL } from "dyna:postgres";
import { suite } from "../lib/kit.js";

// Each case starts its own mock server and client and closes both; nothing
// listens under --list.
const t = suite("test_postgres_postgre_sql_fragment");
let A = null;
const check = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);

function bytes(s) {
  const a = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff;
  return a;
}
function latin1(u8) {
  let s = "";
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return s;
}

function dribble(c, s, at) {
  const pieces = [];
  if (at === -1) {
    for (let i = 0; i < s.length; i++) pieces.push(s[i]);
  } else {
    let prev = 0;
    for (const p of at) { pieces.push(s.slice(prev, p)); prev = p; }
    pieces.push(s.slice(prev));
  }
  let k = 0;
  (function send() {
    if (k >= pieces.length) return;
    const piece = pieces[k++];
    try { if (piece.length) c.write(bytes(piece)); } catch (e) { return; }
    setTimeout(send, 1);
  })();
}

function i32(v) {
  return String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255,
                             (v >>> 8) & 255, v & 255);
}
function i16(v) { return String.fromCharCode((v >>> 8) & 255, v & 255); }
function cstr(s) { return s + "\0"; }
function msg(t, b) { return t + i32(b.length + 4) + b; }

function pgServer(mode) {
  const bufs = new Map(), started = new Map();
  const srv = new TCPServer({ port: 0 });
  srv.start({
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + latin1(b));
      for (;;) {
        const s = bufs.get(c);
        if (!started.get(c)) {
          if (s.length < 8) return;
          const len = ((s.charCodeAt(0) << 24) | (s.charCodeAt(1) << 16) |
                       (s.charCodeAt(2) << 8) | s.charCodeAt(3)) >>> 0;
          if (s.length < len) return;
          bufs.set(c, s.slice(len));
          started.set(c, true);
          c.write(bytes(msg("R", i32(0)) + msg("K", i32(9) + "abcd") +
                        msg("Z", "I")));
          continue;
        }
        if (s.length < 5) return;
        const len = ((s.charCodeAt(1) << 24) | (s.charCodeAt(2) << 16) |
                     (s.charCodeAt(3) << 8) | s.charCodeAt(4)) >>> 0;
        if (s.length < len + 1) return;
        const type = s[0];
        bufs.set(c, s.slice(len + 1));
        if (type !== "Q") continue;
        const val = "row payload that is long enough to be split many times";
        const reply =
          msg("T", i16(2) + cstr("a") + i32(0) + i16(0) + i32(25) +
                   i16(0xffff) + i32(0xffffffff) + i16(0) +
                   cstr("b") + i32(0) + i16(0) + i32(23) +
                   i16(0xffff) + i32(0xffffffff) + i16(0)) +
          msg("D", i16(2) + i32(val.length) + val + i32(3) + "777") +
          msg("C", cstr("SELECT 1")) + msg("Z", "I");
        if (mode === "bytewise") dribble(c, reply, -1);
        else if (mode === "header") {
          const at = reply.indexOf("D", 10) + 3;
          dribble(c, reply, [at]);
        } else if (mode === "batched") {
          c.write(bytes(reply.slice(0, reply.length - 4)));
          setTimeout(() => { try { c.write(bytes(reply.slice(reply.length - 4))); } catch (e) {} }, 20);
        }
      }
    },
    close: (c) => { bufs.delete(c); started.delete(c); },
  });
  return srv;
}

const PGVAL = "row payload that is long enough to be split many times";

async function settle(p) { try { return { ok: await p }; } catch (e) { return { err: e }; } }
const emsgOf = (r) => r && r.err ? String(r.err.message || r.err) : "";
const results = {};
const emsg = (k) => emsgOf(results[k]);

for (const mode of ["bytewise", "header", "batched"])
  test("PostgreSQL reassembles a reply split " + mode, async () => {
    const srv = pgServer(mode);
    const cli = new PostgreSQL({ port: srv.port, host: "127.0.0.1", user: "u",
                               connectTimeoutMs: 20000 });
    const r = await settle(cli.query("SELECT 1"));
    results["p_" + mode] = r;
    cli.close(); srv.close();
    const v = r.ok;
    check(v && v.rows && v.rows.length === 1,
          "PostgreSQL split '" + mode + "': one row, got " +
          (v ? JSON.stringify(v.rows).slice(0, 50) : "error " + emsg("p_" + mode)));
    check(v && v.rows[0] && v.rows[0].a === PGVAL,
          "PostgreSQL split '" + mode + "': the text column must survive " +
          "reassembly");
    check(v && v.rows[0] && v.rows[0].b === 777,
          "PostgreSQL split '" + mode + "': and the column after it, got " +
          (v && v.rows[0] && v.rows[0].b));
    check(v && v.command === "SELECT 1",
          "PostgreSQL split '" + mode + "': the tag arrives after the row");
  }, { timeoutMs: 30000 });

await t.run();
