// timeout: 180
import { TCPServer, Redis } from "dyna:net";

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
  if (s.length === 0) return null;
  if (s[0] !== "*") return null;
  let i = s.indexOf("\r\n");
  if (i < 0) return null;
  const count = parseInt(s.slice(1, i), 10);
  let p = i + 2;
  const args = [];
  for (let k = 0; k < count; k++) {
    if (p >= s.length) return null;
    if (s[p] !== "$") return null;
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
      if (cmd === "HELLO") {
        c.write(bytes("%1\r\n" + bulk("proto") + ":3\r\n"));
      } else if (cmd === "PING") {
        c.write(bytes("+PONG\r\n"));
      } else if (cmd === "RAW") {
        c.write(bytes(one.args.length > 1 ? one.args[1] : ""));
      } else if (cmd === "SPLIT") {
        const payload = one.args[1] || "";
        const half = Math.floor(payload.length / 2);
        c.write(bytes(payload.slice(0, half)));
        c.write(bytes(payload.slice(half)));
      } else {
        c.write(bytes("-ERR unknown command\r\n"));
      }
    }
  },
  close: (c) => { bufs.delete(c); },
});

function client(opts) {
  const bag = { port: srv.port, host: "127.0.0.1",
                connectTimeoutMs: 2000, commandTimeoutMs: 2000 };
  for (const k in (opts || {})) bag[k] = opts[k];
  const r = new Redis(bag);
  r.on("error", () => {});
  return r;
}
async function raw(opts, wire) {
  const r = client(opts);
  try {
    const v = await r.command("RAW", wire);
    return { v };
  } catch (e) {
    return { err: e };
  } finally {
    r.close();
  }
}

const I64MAX = 9223372036854775807n;
const I64MIN = -9223372036854775808n;

const GOOD = [
  [":0\r\n",                       0n],
  [":-0\r\n",                      0n],
  [":+0\r\n",                      0n],
  [":1\r\n",                       1n],
  [":-1\r\n",                     -1n],
  [":+1\r\n",                      1n],
  [":7\r\n",                       7n],
  [":-7\r\n",                      -7n],
  [":000000000000000000000000001\r\n", 1n],
  [":-000000000000000000000000001\r\n", -1n],
  [":2147483647\r\n",              2147483647n],
  [":-2147483648\r\n",             -2147483648n],
  [":-2147483649\r\n",             -2147483649n],
  [":4294967295\r\n",              4294967295n],
  [":-4294967296\r\n",             -4294967296n],
  [":4294967296\r\n",              4294967296n],
  [":-4294967297\r\n",             -4294967297n],
  [":9007199254740992\r\n",        9007199254740992n],
  [":9007199254740993\r\n",        9007199254740993n],
  [":-9007199254740993\r\n",      -9007199254740993n],
  [":9223372036854775806\r\n",     9223372036854775806n],
  [":9223372036854775807\r\n",     I64MAX],
  [":-9223372036854775807\r\n",    -9223372036854775807n],
  [":-9223372036854775808\r\n",    I64MIN],
  [":-9223372036854775808\r\n",    I64MIN],
];

const BAD = [
  [":9223372036854775808\r\n",    "2^63 positive"],
  [":9223372036854775809\r\n",    "2^63 + 1"],
  [":18446744073709551615\r\n",   "UINT64_MAX"],
  [":18446744073709551616\r\n",   "2^64"],
  [":-9223372036854775809\r\n",   "INT64_MIN - 1"],
  [":-18446744073709551615\r\n",  "-UINT64_MAX"],
  [":-18446744073709551616\r\n",  "-2^64"],
  [":-99999999999999999999999\r\n", "23 digits"],
  [":-00000000000000000009223372036854775809\r\n", "padded past the edge"],
  [":" + "9".repeat(200) + "\r\n", "200 nines"],
  [":" + "1".repeat(40) + "\r\n",   "40 ones"],
];

const JUNK = [
  [": \r\n",           "a leading space"],
  [":1 2\r\n",         "an embedded space"],
  [":1\t2\r\n",        "an embedded tab"],
  [":--1\r\n",         "a doubled sign"],
  [":+-1\r\n",         "a sign after a sign"],
  [":-\r\n",           "a sign with no digits"],
  [":+\r\n",           "likewise"],
  [":0x10\r\n",        "hex is not RESP"],
  [":1_000\r\n",       "separators are not RESP"],
  [":١٢٣\r\n",         "non-ASCII digits"],
  [":\r\n",            "an empty field"],
  [":12x4\r\n",        "a trailing letter"],
  [":1\n",             "a bare LF (one reply read as two)"],
  [":-9223372036854775808", "a truncated INT64_MIN (more bytes needed)"],
];

{
  const r = client();
  let pong = null;
  try { pong = await r.command("PING"); } catch (e) { pong = "threw: " + e; }
  const zero = await raw({ bigint: true }, ":0\r\n");
  if (pong !== "PONG" || zero.err || String(zero.v) !== "0") {
    print("CONTROL FAILED: PING=" + JSON.stringify(pong) +
          " RAW(:0)=" + (zero.err ? "threw " + zero.err : String(zero.v)) +
          " -- the harness is broken, the rest of this run proves nothing");
    throw new Error("test_net_resp_int: control failed, run voided");
  }
  r.close();
  check(true, "control: the mock answers HELLO and a legal integer");
}

{
  const r = client({ bigint: true });
  for (let i = 0; i < GOOD.length; i++) {
    const wire = GOOD[i][0], want = GOOD[i][1];
    let got, err = null;
    try { got = await r.command("RAW", wire); }
    catch (e) { err = e; }
    const shown = wire.replace(/\r\n$/, "").replace(/^:/, "");
    check(err === null,
          shown + ": must decode, threw " + (err ? (err.message || err) : "?"));
    check(err === null && typeof got === "bigint",
          shown + ": with bigint:true must be a BigInt, got " + typeof got);
    check(err === null && got === want,
          shown + ": decoded to " + String(got) + ", want " + String(want));
  }
  let alive = null;
  try { alive = await r.command("PING"); } catch (e) { alive = "threw: " + e; }
  check(alive === "PONG", "the client must still be usable after INT64_MIN, got " + alive);
  r.close();
}

{
  for (let i = 0; i < GOOD.length; i++) {
    const wire = GOOD[i][0], want = GOOD[i][1];
    const digits = wire.slice(1, -2);
    const r = raw({}, wire);
    let res = null;
    try { res = await r; } catch (e) {  }
    if (res.err) {
      check(false, digits + " (text mode): threw " + (res.err.message || res.err));
    } else {
      const wantBig = want > 9007199254740992n || want < -9007199254740992n;
      const ok = wantBig ? res.v === digits : res.v === Number(want);
      check(ok, digits + " (text mode): got " + JSON.stringify(res.v) +
            ", want " + (wantBig ? JSON.stringify(digits) : String(Number(want))));
    }
  }
}

for (let i = 0; i < BAD.length; i++) {
  const wire = BAD[i][0], name = BAD[i][1];
  const res = await raw({ bigint: true }, wire);
  check(!!res.err, name + ": must be REFUSED, decoded to " +
        (res.err ? "?" : JSON.stringify(res.v)));
  const msg = res.err ? String(res.err.message || res.err) : "";
  check(!!res.err && /integer out of range/.test(msg),
        name + ": must name the INTEGER range, got " + JSON.stringify(msg));
  check(!!res.err && !/length past its limit/.test(msg),
        name + ": must NOT be reported as a length problem, got " + JSON.stringify(msg));
}

{
  const r = client();
  let pong = null;
  try { pong = await r.command("PING"); } catch (e) { pong = "threw: " + e; }
  check(pong === "PONG", "a client made after a refusal must still work, got " + pong);
  r.close();
}

for (let i = 0; i < JUNK.length; i++) {
  const wire = JUNK[i][0], name = JUNK[i][1];
  const res = await raw({ bigint: true }, wire);
  const msg = res.err ? String(res.err.message || res.err) : "";
  if (name.startsWith("a truncated")) {
    check(!!res.err && /timed out|timeout/.test(msg),
          name + ": a partial reply must WAIT for the rest, got " + JSON.stringify(msg));
    continue;
  }
  check(!!res.err, name + ": must be REFUSED, got " + JSON.stringify(res.v));
  check(!!res.err && !/integer out of range/.test(msg),
        name + ": is a syntax problem, not a range problem: " + JSON.stringify(msg));
}

{
  let res = await raw({ bigint: true }, ":0\r\n");
  check(!res.err && res.v === 0n, "control before the split-read rows");
  const parts = [[":-922337203685477", "5808\r\n"],
                 [":922337203685477", "5808\r\n"],
                 [":922337203685477", "5807\r\n"],
                 [":-922337203685477", "5809\r\n"]];
  for (let i = 0; i < parts.length; i++) {
    const want = parts[i][0] + parts[i][1];
    const r = client({ bigint: true });
    let got, err = null;
    try { got = await r.command("SPLIT", parts[i][0] + parts[i][1]); }
    catch (e) { err = e; }
    r.close();
    if (want === ":-9223372036854775808\r\n")
      check(err === null && got === I64MIN,
            "a SPLIT INT64_MIN must still decode, got " +
            (err ? "threw " + (err.message || err) : String(got)));
    else if (want === ":9223372036854775807\r\n")
      check(err === null && got === I64MAX,
            "a SPLIT INT64_MAX must still decode, got " +
            (err ? "threw " + (err.message || err) : String(got)));
    else
      check(!!err, "a SPLIT " + want.trim() + " must be refused, got " + String(got));
  }
}

{
  const zeros = "0".repeat(40000);
  const res = await raw({ bigint: true }, ":-" + zeros + "1\r\n");
  check(!res.err && res.v === -1n,
        "40k leading zeros then 1 is -1, not a bomb: got " +
        (res.err ? "threw " + (res.err.message || res.err) : String(res.v)));
  const res2 = await raw({ bigint: true }, ":" + zeros + "1\r\n");
  check(!res2.err && res2.v === 1n,
        "and positive, likewise: got " +
        (res2.err ? "threw " + (res2.err.message || res2.err) : String(res2.v)));
  const wide = "1".repeat(70000);
  const res3 = await raw({ bigint: true }, ":" + wide);
  const msg3 = res3.err ? String(res3.err.message || res3.err) : "";
  check(!!res3.err && /length past its limit/.test(msg3),
        "a 70k-digit run past the 64 KiB line cap is a LENGTH refusal: " +
        JSON.stringify(msg3));
  const under = "1".repeat(60000);
  const res3b = await raw({ bigint: true }, ":" + under);
  const m3b = res3b.err ? String(res3b.err.message || res3b.err) : "";
  check(!!res3b.err && /timed out|timeout/.test(m3b),
        "a 60k-digit run with no CRLF must WAIT for the rest, not be refused: " +
        JSON.stringify(m3b));
  const res4 = await raw({ bigint: true }, ":" + wide.slice(0, 40) + "\r\n");
  check(!!res4.err && /integer out of range/.test(
          String(res4.err && (res4.err.message || res4.err))),
        "but 40 digits of ones IS out of range: got " +
        JSON.stringify(res4.err ? String(res4.err.message || res4.err) : res4.v));
}

{
  const res = await raw({}, "$-9223372036854775808\r\n");
  const msg = res.err ? String(res.err.message || res.err) : "";
  check(!!res.err, "$-INT64_MIN is a LENGTH and must not decode to a value");
  check(!!res.err && !/integer out of range/.test(msg),
        "and $-1 is the only negative length RESP2 has: " + JSON.stringify(msg));
  const res2 = await raw({}, "*-9223372036854775808\r\n");
  check(!!res2.err, "*-INT64_MIN likewise");
  const res3 = await raw({}, "$9223372036854775807\r\n");
  const m3 = res3.err ? String(res3.err.message || res3.err) : "";
  check(!!res3.err && /length past its limit/.test(m3),
        "INT64_MAX as a length is representable, so maxbulk refuses it: " + JSON.stringify(m3));
  const res4 = await raw({}, "*9223372036854775808\r\n");
  const m4 = res4.err ? String(res4.err.message || res4.err) : "";
  check(!!res4.err && /integer out of range/.test(m4),
        "and 2^63 does not fit at all, so the INTEGER guard is what refuses: " +
        JSON.stringify(m4));
}

{
  const live = new Redis({ host: "127.0.0.1", port: 6379, bigint: true,
                           connectTimeoutMs: 400, commandTimeoutMs: 2000 });
  live.on("error", () => {});
  let up = false;
  try { up = (await live.command("PING")) === "PONG"; } catch (e) { up = false; }
  if (!up) {
    print("SKIP: no redis on 127.0.0.1:6379 -- the live second opinion did not run");
    live.close();
  } else {
    const LUA = [
      ["return 0",                        0n],
      ["return -1",                      -1n],
      ["return -2147483648",       -2147483648n],
      ["return 2147483647",         2147483647n],
      ["return 9223372036854775807", I64MAX],
      ["return -9223372036854775807", I64MIN],
      ["return -9223372036854775808", I64MIN],
    ];
    for (let i = 0; i < LUA.length; i++) {
      let got, err = null;
      try { got = await live.command("EVAL", LUA[i][0], "0"); }
      catch (e) { err = e; }
      check(err === null, "live EVAL " + LUA[i][0] + ": threw " +
            (err ? (err.message || err) : "?"));
      check(err === null && got === LUA[i][1],
            "live EVAL " + LUA[i][0] + ": got " + String(got) +
            ", want " + String(LUA[i][1]));
    }
    let alive = null;
    try { alive = await live.command("PING"); } catch (e) { alive = "threw: " + e; }
    check(alive === "PONG", "the live connection must survive INT64_MIN, got " + alive);

    const key = "dynajs:test:resp-int:" + String(Date.now());
    let under = null, underErr = null;
    try {
      await live.command("SET", key, "-9223372036854775807");
      under = await live.command("DECRBY", key, "1");
      await live.command("DEL", key);
    } catch (e) { underErr = e; }
    check(underErr === null && under === I64MIN,
          "a DECRBY underflow must decode to INT64_MIN, got " +
          (underErr ? "threw " + (underErr.message || underErr) : String(under)));
    live.close();
  }
}

srv.close();

if (fails === 0) print("test_net_resp_int: all " + n + " checks passed");
else {
  print("test_net_resp_int: " + fails + " FAILED of " + n);
  throw new Error("test_net_resp_int: " + fails + " of " + n + " checks failed");
}
