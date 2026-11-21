// timeout: 120
// covers: dyna:redis.Redis
// flags: --std
// RESP3 streamed strings ($? + ;len chunks, ;0 end) and streamed aggregates (*? ~? %? |?, . end) per the RESP3 spec.
// A fake server echoes raw wire bytes; every streamed reply must decode exactly like its fixed-length form (differential),
// hostile shapes (odd map, streamed push/verbatim/blob-error, '.' outside a stream, NUL bytes) and the
// maxReplyBytes / depth limits at 0, max and max+1 must be refused by name; a 1-byte dribble must reassemble.
// Mutant: canonicalisation skipped in redis-connection.c -> red here (codec mutants are proven in test_resp_codec.c).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_redis_redis_resp_stream");
import { TCPServer } from "dyna:net";
import { Redis } from "dyna:redis";
import * as std from "std";

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
function parseCmd(s) {
  if (s.length === 0 || s[0] !== "*") return null;
  const i = s.indexOf("\r\n");
  if (i < 0) return null;
  const count = parseInt(s.slice(1, i), 10);
  let p = i + 2;
  const args = [];
  for (let k = 0; k < count; k++) {
    if (p >= s.length || s[p] !== "$") return null;
    const j = s.indexOf("\r\n", p);
    if (j < 0) return null;
    const len = parseInt(s.slice(p + 1, j), 10);
    if (s.length < j + 2 + len + 2) return null;
    args.push(s.slice(j + 2, j + 2 + len));
    p = j + 2 + len + 2;
  }
  return { args, used: p };
}
function bulk(s) { return "$" + s.length + "\r\n" + s + "\r\n"; }
function dribble(c, s) {
  let k = 0;
  (function send() {
    if (k >= s.length) return;
    try { c.write(bytes(s[k++])); } catch (e) { return; }
    setTimeout(send, 1);
  })();
}

const srv = new TCPServer({ port: 0 });
const bufs = new Map();
srv.start({
  data: (c, b) => {
    bufs.set(c, (bufs.get(c) || "") + latin1(b));
    for (;;) {
      const one = parseCmd(bufs.get(c));
      if (!one) return;
      bufs.set(c, bufs.get(c).slice(one.used));
      const cmd = one.args[0].toUpperCase();
      if (cmd === "HELLO") c.write(bytes("%1\r\n" + bulk("proto") + ":3\r\n"));
      else if (cmd === "PING") c.write(bytes("+PONG\r\n"));
      else if (cmd === "RAW") c.write(bytes(one.args.length > 1 ? one.args[1] : ""));
      else if (cmd === "DRIBBLE") dribble(c, one.args[1]);
      else c.write(bytes("-ERR unknown command\r\n"));
    }
  },
  close: (c) => { bufs.delete(c); },
});

function client(extra) {
  const r = new Redis(Object.assign({ port: srv.port, host: "127.0.0.1",
                                      connectTimeoutMs: 2000, commandTimeoutMs: 2000 }, extra || {}));
  r.on("error", () => {});
  return r;
}
async function raw(wire, extra, cmd) {
  const r = client(extra);
  try {
    return { v: await r.command(cmd || "RAW", wire) };
  } catch (e) {
    return { err: e };
  } finally {
    r.close();
  }
}
function msgOf(res) { return res.err ? String(res.err.message || res.err) : ""; }
function show(v) {
  if (v instanceof Map) return "Map{" + [...v].map(([k, x]) => show(k) + "=>" + show(x)).join(",") + "}";
  if (v instanceof Set) return "Set{" + [...v].map(show).join(",") + "}";
  if (v instanceof Uint8Array) return "U8[" + Array.from(v).join(",") + "]";
  if (Array.isArray(v)) return "[" + v.map(show).join(",") + "]";
  if (v && typeof v === "object") return "{" + Object.keys(v).map((k) => k + ":" + show(v[k])).join(",") + "}";
  return typeof v + ":" + String(v);
}

const SAME = [
  ["*?\r\n:1\r\n:2\r\n.\r\n", "*2\r\n:1\r\n:2\r\n", "streamed array"],
  ["*?\r\n.\r\n", "*0\r\n", "empty streamed array"],
  ["~?\r\n+x\r\n+y\r\n.\r\n", "~2\r\n+x\r\n+y\r\n", "streamed set"],
  ["%?\r\n+a\r\n:1\r\n+b\r\n:2\r\n.\r\n", "%2\r\n+a\r\n:1\r\n+b\r\n:2\r\n", "streamed map"],
  ["%?\r\n.\r\n", "%0\r\n", "empty streamed map"],
  ["|?\r\n+k\r\n:0\r\n.\r\n:7\r\n", "|1\r\n+k\r\n:0\r\n:7\r\n", "streamed attribute then its value"],
  ["$?\r\n;4\r\nHell\r\n;5\r\no wor\r\n;1\r\nd\r\n;0\r\n", "$10\r\nHello word\r\n", "chunked string"],
  ["$?\r\n;0\r\n", "$0\r\n\r\n", "empty chunked string"],
  ["$?\r\n;3\r\na\0b\r\n;1\r\n\0\r\n;0\r\n", "$4\r\na\0b\0\r\n", "chunks carrying NUL bytes"],
  ["$?\r\n;2\r\n\r\n\r\n;0\r\n", "$2\r\n\r\n\r\n", "a chunk that is CRLF"],
  ["*?\r\n*?\r\n$?\r\n;2\r\nab\r\n;0\r\n.\r\n%?\r\n.\r\n$-1\r\n_\r\n.\r\n",
   "*4\r\n*1\r\n$2\r\nab\r\n%0\r\n$-1\r\n_\r\n", "nested streams with nulls"],
  ["*?\r\n".repeat(30) + ":7\r\n" + ".\r\n".repeat(30), "*1\r\n".repeat(30) + ":7\r\n", "30 nested streams"],
  ["*?\r\n".repeat(31) + "*?\r\n.\r\n" + ".\r\n".repeat(31), "*1\r\n".repeat(31) + "*0\r\n", "an empty 32nd stream, like a fixed *0"],
];

await __kitS.section("control", async () => {
  const res = await raw("+OK\r\n");
  if (res.err || res.v !== "OK") throw new Error("control failed: the mock is broken, run voided");
  check(true, "control");
});

await __kitS.section("streamed replies decode exactly like their fixed form", async () => {
  for (const [s, f, label] of SAME) {
    const a = await raw(s), b = await raw(f);
    check(!a.err && !b.err && show(a.v) === show(b.v),
          label + ": streamed " + (a.err ? "threw " + msgOf(a) : show(a.v)) + " vs fixed " +
          (b.err ? "threw " + msgOf(b) : show(b.v)));
    const bs = await raw(s, { binary: true }), bf = await raw(f, { binary: true });
    check(!bs.err && show(bs.v) === show(bf.v), label + " (binary): " + show(bs.v) + " vs " + show(bf.v));
  }
});

await __kitS.section("a 1-byte dribble reassembles", async () => {
  for (const [s, f, label] of SAME.slice(0, 11)) {
    const a = await raw(s, null, "DRIBBLE"), b = await raw(f);
    check(!a.err && show(a.v) === show(b.v), label + " dribbled: " + (a.err ? msgOf(a) : show(a.v)));
  }
});

await __kitS.section("hostile shapes are refused by name", async () => {
  const BAD = [
    ["%?\r\n+a\r\n.\r\n", /malformed RESP/, "odd element count in a streamed map"],
    ["|?\r\n+a\r\n.\r\n:1\r\n", /malformed RESP/, "odd element count in a streamed attribute"],
    [">?\r\n.\r\n", /malformed RESP/, "a streamed push"],
    ["!?\r\n;0\r\n", /malformed RESP/, "a streamed blob error"],
    ["=?\r\n;0\r\n", /malformed RESP/, "a streamed verbatim string"],
    ["$?\r\n;-1\r\n", /malformed RESP/, "a negative chunk"],
    ["$?\r\nabc\r\n", /malformed RESP/, "a chunk without ;"],
    ["$?\r\n;3\r\nabcX\r\n;0\r\n", /malformed RESP/, "a chunk without its CRLF"],
    ["*2\r\n:1\r\n.\r\n", /unknown RESP type/, "'.' inside a fixed array"],
    [".\r\n", /unknown RESP type/, "a bare '.'"],
    ["*?\r\n".repeat(32) + ":7\r\n" + ".\r\n".repeat(32), /nesting past its limit/, "32 nested streams"],
    ["*?\r\n".repeat(31) + "|?\r\n.\r\n:1\r\n" + ".\r\n".repeat(31), /nesting past its limit/, "an attribute stream at depth 32"],
  ];
  for (const [w, re, label] of BAD) {
    const res = await raw(w);
    check(!!res.err && re.test(msgOf(res)), label + " must be refused by name, got " +
          (res.err ? JSON.stringify(msgOf(res)) : "value " + show(res.v)));
  }
});

await __kitS.section("maxReplyBytes bounds chunk totals and stream counts at the edge", async () => {
  const lim = { maxReplyBytes: 9 };
  const ok3 = await raw("*?\r\n:1\r\n:2\r\n:3\r\n.\r\n", lim);
  check(!ok3.err && show(ok3.v) === show([1, 2, 3]), "3 elements at cap 9/3 must decode, got " + msgOf(ok3));
  const bad4 = await raw("*?\r\n:1\r\n:2\r\n:3\r\n:4\r\n.\r\n", lim);
  check(!!bad4.err && /element count/.test(msgOf(bad4)), "4 elements past cap 9/3 must be refused, got " + msgOf(bad4));
  const ok9 = await raw("$?\r\n;4\r\nabcd\r\n;5\r\nefghi\r\n;0\r\n", lim);
  check(!ok9.err && ok9.v === "abcdefghi", "a 9-byte chunk total at cap 9 must decode, got " + msgOf(ok9));
  const bad10 = await raw("$?\r\n;4\r\nabcd\r\n;6\r\nefghij\r\n;0\r\n", lim);
  check(!!bad10.err && /length past its limit/.test(msgOf(bad10)), "a 10-byte chunk total must be refused, got " + msgOf(bad10));
  const z = await raw("$?\r\n;0\r\n", { maxReplyBytes: 6 });
  check(!z.err && z.v === "", "an empty chunked string under the smallest cap HELLO allows (6) must decode, got " + msgOf(z));
  const six = await raw("$?\r\n;6\r\nabcdef\r\n;0\r\n", { maxReplyBytes: 6 });
  check(!six.err && six.v === "abcdef", "one 6-byte chunk at cap 6 must decode, got " + msgOf(six));
  const seven = await raw("$?\r\n;6\r\nabcdef\r\n;1\r\ng\r\n;0\r\n", { maxReplyBytes: 6 });
  check(!!seven.err && /length past its limit/.test(msgOf(seven)), "a 7-byte chunk total at cap 6 must be refused, got " + msgOf(seven));
});

await __kitS.section("an unterminated stream waits instead of settling", async () => {
  for (const w of ["*?\r\n:1\r\n", "$?\r\n;2\r\nab\r\n", "|?\r\n.\r\n", "*?\r\n".repeat(31) + "*?\r\n."]) {
    const res = await raw(w, { commandTimeoutMs: 300 });
    check(!!res.err && /timed out|timeout/.test(msgOf(res)), JSON.stringify(w) + " must time out, got " +
          (res.err ? msgOf(res) : show(res.v)));
  }
});

await __kitS.section("streamed and fixed replies interleave on one connection", async () => {
  const r = client();
  const got = [];
  try {
    const ps = [];
    for (let i = 0; i < 40; i++) {
      ps.push(r.command("RAW", i % 2 ? "*?\r\n:" + i + "\r\n.\r\n" : "*1\r\n:" + i + "\r\n"));
      if (i % 7 === 0) std.gc();
    }
    for (const p of ps) got.push((await p)[0]);
  } catch (e) { got.push("threw " + e); }
  r.close();
  check(typeof std.gc === "function", "std.gc must exist so the in-flight GC step is real");
  check(got.length === 40 && got.every((v, i) => v === i), "40 interleaved replies in order, got " + show(got));
});

srv.close();
print(n + " checks, " + fails + " failed");
__kitS.end();
