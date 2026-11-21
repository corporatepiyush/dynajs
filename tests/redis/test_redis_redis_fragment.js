// covers: dyna:redis.Redis
// Split from tests/net/test_net_fragment.js (one module per suite).
import { TCPServer } from "dyna:net";
import { Redis } from "dyna:redis";
import { suite } from "../lib/kit.js";

// Each case starts its own mock server and client and closes both; nothing
// listens under --list.
const t = suite("test_redis_redis_fragment");
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
        const payload = "the quick brown fox jumps over the lazy dog".repeat(3);
        const reply = "*3\r\n" + bulk(payload) + ":1234567\r\n" + bulk("tail");
        if (mode === "bytewise") dribble(c, reply, -1);
        else if (mode === "crlf") {
          const at = reply.indexOf("\r\n") + 1;
          dribble(c, reply, [at]);
        } else if (mode === "digits") {
          dribble(c, reply, [reply.indexOf("$") + 2]);
        } else if (mode === "batched") {
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

const PAYLOAD = "the quick brown fox jumps over the lazy dog".repeat(3);

async function settle(p) { try { return { ok: await p }; } catch (e) { return { err: e }; } }
const emsgOf = (r) => r && r.err ? String(r.err.message || r.err) : "";
const results = {};
const emsg = (k) => emsgOf(results[k]);

for (const mode of ["bytewise", "crlf", "digits", "batched"])
  test("Redis reassembles a reply split " + mode, async () => {
    const srv = redisServer(mode);
    const cli = new Redis({ port: srv.port, host: "127.0.0.1",
                            connectTimeoutMs: 20000, commandTimeoutMs: 20000 });
    const r = await settle(cli.command("GET", "k"));
    results["r_" + mode] = r;
    cli.close(); srv.close();
    const v = r.ok;
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
  }, { timeoutMs: 30000 });

await t.run();
