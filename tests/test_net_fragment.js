/* test_net_fragment.js -- the REASSEMBLY loops, driven byte by byte.
 *
 * The fuzzer drives the parsers with whole buffers, so it never touches the
 * code between the socket and the parser: `redis_on_recv` and `pg_on_recv`
 * accumulate, rescan, and memmove the remainder down. That buffering is its own
 * bug surface -- an off-by-one in the compaction, a `rpos` not reset, a rescan
 * from the wrong offset -- and none of it is reachable unless a reply actually
 * ARRIVES IN PIECES.
 *
 * Loopback will not fragment on its own: two writes issued together land in one
 * segment. Every split here is therefore forced with a timer, which is the only
 * thing that makes the partial-read path real. That is the same technique the
 * DNS length-prefix window needed, and it found a bug there.
 *
 * The splits are chosen to land where the code branches:
 *   - one byte at a time, so every intermediate state is visited;
 *   - between the CR and the LF of a terminator;
 *   - inside the digits of a length prefix;
 *   - inside a message header, which for PostgreSQL is the `avail < 5` path;
 *   - several complete replies in ONE write, then a partial one, which is the
 *     case where the remainder must be moved down and rescanned.
 */
import { TCPServer, Redis, PostgreSQL } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

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

/* Write `s` in the given pieces, each in its own event-loop turn. `at` is a
 * list of absolute offsets to cut at; -1 means "one byte at a time". */
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

/* ---- Redis ------------------------------------------------------------ */

function bulk(s) { return "$" + s.length + "\r\n" + s + "\r\n"; }

function redisServer(mode) {
  const bufs = new Map();
  const srv = new TCPServer({ port: 0 });
  srv.start({
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + latin1(b));
      for (;;) {
        const s = bufs.get(c);
        if (s[0] !== "*") return;
        const i = s.indexOf("\r\n");
        if (i < 0) return;
        const cnt = parseInt(s.slice(1, i), 10);
        let p = i + 2;
        const args = [];
        let ok = true;
        for (let k = 0; k < cnt; k++) {
          const j = s.indexOf("\r\n", p);
          if (j < 0) { ok = false; break; }
          const l = parseInt(s.slice(p + 1, j), 10);
          if (s.length < j + 2 + l + 2) { ok = false; break; }
          args.push(s.slice(j + 2, j + 2 + l));
          p = j + 2 + l + 2;
        }
        if (!ok) return;
        bufs.set(c, s.slice(p));
        const cmd = args[0].toUpperCase();
        if (cmd === "HELLO") { c.write(bytes("%1\r\n" + bulk("proto") + ":3\r\n")); continue; }
        /* A reply with a long payload, so a split can land anywhere in it. */
        const payload = "the quick brown fox jumps over the lazy dog".repeat(3);
        const reply = "*3\r\n" + bulk(payload) + ":1234567\r\n" + bulk("tail");
        if (mode === "bytewise") dribble(c, reply, -1);
        else if (mode === "crlf") {
          /* cut between the CR and the LF of the first terminator */
          const at = reply.indexOf("\r\n") + 1;
          dribble(c, reply, [at]);
        } else if (mode === "digits") {
          /* cut inside the digits of the bulk length */
          dribble(c, reply, [reply.indexOf("$") + 2]);
        } else if (mode === "batched") {
          /* two complete replies, then HALF of a third: the remainder must be
             moved down and rescanned when the rest arrives */
          const two = reply + reply;
          c.write(bytes(two + reply.slice(0, 20)));
          setTimeout(() => { try { c.write(bytes(reply.slice(20))); } catch (e) {} }, 20);
        }
      }
    },
    close: (c) => { bufs.delete(c); },
  });
  return srv;
}

/* ---- PostgreSQL --------------------------------------------------------- */

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
          /* cut INSIDE the 5-byte header of the DataRow: the `avail < 5` path */
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

/* ---- drive ------------------------------------------------------------ */

const PAYLOAD = "the quick brown fox jumps over the lazy dog".repeat(3);
const PGVAL = "row payload that is long enough to be split many times";

const results = {};
function rec(k, p) {
  results[k] = "pending";
  p.then((v) => { results[k] = { ok: v }; }, (e) => { results[k] = { err: e }; });
}

const R = {};
for (const mode of ["bytewise", "crlf", "digits", "batched"]) {
  const srv = redisServer(mode);
  const cli = new Redis({ port: srv.port, host: "127.0.0.1",
                          connectTimeoutMs: 20000, commandTimeoutMs: 20000 });
  R["r_" + mode] = { srv, cli };
  rec("r_" + mode, cli.command("GET", "k"));
}
for (const mode of ["bytewise", "header", "batched"]) {
  const srv = pgServer(mode);
  const cli = new PostgreSQL({ port: srv.port, host: "127.0.0.1", user: "u",
                             connectTimeoutMs: 20000 });
  R["p_" + mode] = { srv, cli };
  rec("p_" + mode, cli.query("SELECT 1"));
}

function settled() {
  for (const k in results) if (results[k] === "pending") return false;
  return true;
}

let spins = 0;
const t = setInterval(() => {
  if (!settled() && spins++ < 3000) return;
  clearInterval(t);

  const val = (k) => results[k] && results[k].ok;
  const emsg = (k) => results[k] && results[k].err
                    ? String(results[k].err.message || results[k].err) : "";

  for (const mode of ["bytewise", "crlf", "digits", "batched"]) {
    const v = val("r_" + mode);
    check(Array.isArray(v) && v.length === 3,
          "Redis split '" + mode + "': expected a 3-element reply, got " +
          (v ? JSON.stringify(v).slice(0, 60) : "error " + emsg("r_" + mode)));
    check(v && v[0] === PAYLOAD,
          "Redis split '" + mode + "': the bulk payload must survive " +
          "reassembly byte for byte");
    check(v && v[1] === 1234567,
          "Redis split '" + mode + "': the integer after it, got " +
          (v && v[1]));
    check(v && v[2] === "tail",
          "Redis split '" + mode + "': and the element after THAT, which is " +
          "what a mis-sized compaction loses");
  }

  for (const mode of ["bytewise", "header", "batched"]) {
    const v = val("p_" + mode);
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
  }

  for (const k in R) { R[k].cli.close(); R[k].srv.close(); }
  if (fails === 0) print("test_net_fragment: all " + n + " checks passed");
  else print("test_net_fragment: " + fails + " FAILED of " + n);
}, 10);
