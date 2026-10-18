// timeout: 180
import { TCPServer, Redis } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const MAX_CONTAINERS = 31;

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
  const i = s.indexOf("\r\n");
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
      if (cmd === "HELLO") c.write(bytes("%1\r\n" + bulk("proto") + ":3\r\n"));
      else if (cmd === "PING") c.write(bytes("+PONG\r\n"));
      else if (cmd === "RAW") c.write(bytes(one.args.length > 1 ? one.args[1] : ""));
      else c.write(bytes("-ERR unknown command\r\n"));
    }
  },
  close: (c) => { bufs.delete(c); },
});

function client() {
  const r = new Redis({ port: srv.port, host: "127.0.0.1",
                        connectTimeoutMs: 2000, commandTimeoutMs: 2000 });
  r.on("error", () => {});
  return r;
}
async function raw(wire) {
  const r = client();
  try {
    return { v: await r.command("RAW", wire) };
  } catch (e) {
    return { err: e };
  } finally {
    r.close();
  }
}
function msgOf(res) { return res.err ? String(res.err.message || res.err) : ""; }

function nestArray(d) { return "*1\r\n".repeat(d) + ":7\r\n"; }
function nestMap(d)  { return "%1\r\n+k\r\n".repeat(d) + ":7\r\n"; }
function nestSet(d)  { return "~1\r\n".repeat(d) + ":7\r\n"; }
const ATTR1 = "|1\r\n$1\r\nk\r\n:0\r\n";
const ATTR0 = "|0\r\n";
function attrChain(d, unit) {
  let s = "";
  for (let i = 0; i < d; i++) s += unit;
  return s + ":7\r\n";
}

{
  const r = client();
  let pong = null;
  try { pong = await r.command("PING"); } catch (e) { pong = "threw: " + e; }
  const shallow = await raw("+OK\r\n");
  if (pong !== "PONG" || shallow.err || shallow.v !== "OK") {
    print("CONTROL FAILED: PING=" + JSON.stringify(pong) +
          " RAW(+OK)=" + (shallow.err ? "threw " + shallow.err : String(shallow.v)) +
          " -- the harness is broken, the rest of this run proves nothing");
    throw new Error("test_net_resp_depth: control failed, run voided");
  }
  r.close();
  check(true, "control: the mock answers HELLO and a legal simple string");
}

for (const [name, make] of [["array", nestArray], ["map", nestMap], ["set", nestSet]]) {
  for (const d of [1, 2, 15, MAX_CONTAINERS - 1, MAX_CONTAINERS]) {
    const res = await raw(make(d));
    check(!res.err,
          name + " nested " + d + " deep must decode, got " + JSON.stringify(msgOf(res)));
  }
  for (const d of [MAX_CONTAINERS + 1, MAX_CONTAINERS + 2, 40, 300, 5000]) {
    const res = await raw(make(d));
    check(!!res.err, name + " nested " + d + " deep must be REFUSED");
    check(/nesting past its limit/.test(msgOf(res)),
          name + " nested " + d + " must be refused BY NAME, got " +
          JSON.stringify(msgOf(res)));
  }
}

for (const [label, unit] of [["empty", ATTR0], ["one-pair", ATTR1]]) {
  for (const d of [1, 2, MAX_CONTAINERS - 1, MAX_CONTAINERS]) {
    const res = await raw(attrChain(d, unit));
    check(!res.err, "a chain of " + d + " " + label + " attributes must decode, got " +
          JSON.stringify(msgOf(res)));
    check(!res.err && res.v === 7,
          "and the reply must be the decorated value 7, got " +
          JSON.stringify(res.err ? msgOf(res) : res.v));
  }
  for (const d of [MAX_CONTAINERS + 1, 32, 40, 300, 5000]) {
    const res = await raw(attrChain(d, unit));
    check(!!res.err, "a chain of " + d + " " + label + " attributes must be REFUSED");
    check(/nesting past its limit/.test(msgOf(res)),
          "and refused BY NAME, got " + JSON.stringify(msgOf(res)));
  }
}

{
  const res = await raw(attrChain(MAX_CONTAINERS + 1, ATTR0));
  check(!/malformed reply/.test(msgOf(res)),
        "an over-deep attribute chain must not be reported as a bare " +
        "'malformed reply', got " + JSON.stringify(msgOf(res)));
}

{
  const res3 = await raw(ATTR0 + nestArray(MAX_CONTAINERS));
  check(!!res3.err,
        "one attribute in front of a 31-deep array is 32 deep and must be refused");
  check(/nesting past its limit/.test(msgOf(res3)),
        "and by name, got " + JSON.stringify(msgOf(res3)));
  const res4 = await raw(ATTR0 + nestArray(MAX_CONTAINERS - 1));
  check(!res4.err, "one attribute in front of a 30-deep array is 31 and must decode, got " +
        JSON.stringify(msgOf(res4)));
  const res5 = await raw(ATTR0 + ATTR0 + nestArray(MAX_CONTAINERS - 2));
  check(!res5.err, "two attributes then a 29-deep array is 31 and must decode, got " +
        JSON.stringify(msgOf(res5)));
  const res6 = await raw(ATTR0 + ATTR0 + nestArray(MAX_CONTAINERS - 1));
  check(!!res6.err && /nesting past its limit/.test(msgOf(res6)),
        "two attributes then a 30-deep array is 32 and must be refused by name, got " +
        JSON.stringify(msgOf(res6)));
}

{
  const r = client();
  let first = null, second = null, err = null;
  try {
    first = await r.command("RAW", attrChain(MAX_CONTAINERS, ATTR0));
    second = await r.command("RAW", attrChain(MAX_CONTAINERS, ATTR0));
  } catch (e) { err = e; }
  r.close();
  check(!err && first === 7 && second === 7,
        "two 31-attribute replies on ONE connection must both decode, got " +
        JSON.stringify(err ? String(err.message || err) : [first, second]));
}

{
  const res = await raw(ATTR0);
  check(!!res.err && /timed out|timeout/.test(msgOf(res)),
        "a bare attribute with nothing after it must WAIT, not settle, got " +
        JSON.stringify(msgOf(res)));
}

{
  const cases = [
    ["*0\r\n",                        "an empty array"],
    ["*1\r\n*0\r\n",                  "an array holding an empty array"],
    [":7\r\n",                        "a scalar after a legal chain"],
    [ATTR0 + ATTR0 + ":7\r\n",        "two attributes then a value"],
    [ATTR1 + ATTR1 + ":7\r\n",        "two one-pair attributes then a value"],
    ["%1\r\n$1\r\na\r\n:1\r\n",      "a one-pair map"],
    [ATTR1 + "%1\r\n$1\r\na\r\n:1\r\n", "a decorated map"],
  ];
  for (let i = 0; i < cases.length; i++) {
    const res = await raw(cases[i][0]);
    check(!res.err, cases[i][1] + " must decode, got " + JSON.stringify(msgOf(res)));
  }
}

srv.close();

if (fails === 0) print("test_net_resp_depth: all " + n + " checks passed");
else {
  print("test_net_resp_depth: " + fails + " FAILED of " + n);
  throw new Error("test_net_resp_depth: " + fails + " of " + n + " checks failed");
}
