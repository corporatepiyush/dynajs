// covers: dyna:redis.Redis
// Split from tests/net/test_net_rss.js (one module per suite).
import { TCPServer } from "dyna:net";
import { Redis } from "dyna:redis";
import { memoryUsage } from "dyna:sys";

import { suite } from "../lib/kit.js";

// The mock Redis server starts in the kit's before hook (never
// under --list); the churn runs once and the two plateau checks read it.
const t = suite("test_redis_redis_rss");
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
function i32(v) {
  return String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255,
                             (v >>> 8) & 255, v & 255);
}
function i16(v) { return String.fromCharCode((v >>> 8) & 255, v & 255); }
function cstr(s) { return s + "\0"; }
function msg(t, b) { return t + i32(b.length + 4) + b; }
function rd32(s, at) {
  return ((s.charCodeAt(at) << 24) | (s.charCodeAt(at + 1) << 16) |
          (s.charCodeAt(at + 2) << 8) | s.charCodeAt(at + 3)) >>> 0;
}

const PAD = "x".repeat(4000);

const rbufs = new Map();
let rsrv = null;
t.before(() => {
rsrv = new TCPServer({ port: 0 });
rsrv.start({ data: (c, b) => {
  rbufs.set(c, (rbufs.get(c) || "") + latin1(b));
  let out = "";
  for (;;) {
    const s = rbufs.get(c);
    if (s[0] !== "*") break;
    const i = s.indexOf("\r\n");
    if (i < 0) break;
    const cnt = parseInt(s.slice(1, i), 10);
    let p = i + 2, ok = true;
    const args = [];
    for (let k = 0; k < cnt; k++) {
      const j = s.indexOf("\r\n", p);
      if (j < 0) { ok = false; break; }
      const l = parseInt(s.slice(p + 1, j), 10);
      if (s.length < j + 2 + l + 2) { ok = false; break; }
      args.push(s.slice(j + 2, j + 2 + l));
      p = j + 2 + l + 2;
    }
    if (!ok) break;
    rbufs.set(c, s.slice(p));
    out += args[0].toUpperCase() === "HELLO"
         ? "%1\r\n$5\r\nproto\r\n:3\r\n"
         : "$" + PAD.length + "\r\n" + PAD + "\r\n";
  }
  if (out) c.write(bytes(out));
}, close: (c) => { rbufs.delete(c); } });

});
t.after(() => { if (rsrv) rsrv.close(); });

const ROUNDS = parseInt(scriptArgs[1] || "80", 10);
const PER_ROUND = 6;
const rss = [], eng = [];
let round = 0;

function oneRound(done) {
  let left = PER_ROUND;
  for (let k = 0; k < PER_ROUND; k++) {
    const R = new Redis({ port: rsrv.port, host: "127.0.0.1" });
    Promise.all([
      R.command("GET", "k").catch(() => {}),
      R.pipeline([["GET", "a"], ["GET", "b"]]).catch(() => {}),
    ]).then(() => {
      R.close();
      if (--left === 0) done();
    });
  }
}

const CHURN = "the client churn runs every round";
let M = null;
test(CHURN, async () => {
  while (round++ < ROUNDS) {
    await new Promise((r) => oneRound(r));
    const m = memoryUsage();
    rss.push(m.peakRss);
    eng.push(m.mallocSize);
  }
  const half = rss.length >> 1;
  M = { half, firstRss: rss[half - 1], lastRss: rss[rss.length - 1],
        firstEng: eng[half - 1], lastEng: eng[eng.length - 1],
        ops: (rss.length - half) * PER_ROUND };
  M.grewRss = M.lastRss - M.firstRss;
  M.grewEng = M.lastEng - M.firstEng;
  print("rounds=" + rss.length + " x " + PER_ROUND + " clients");
  print("  peakRss   second half: " + M.firstRss + " -> " + M.lastRss +
        "  (+" + M.grewRss + " bytes over " + M.ops + " lifecycles)");
  print("  engine malloc         : " + M.firstEng + " -> " + M.lastEng +
        "  (+" + M.grewEng + ")");
  A.eq(rss.length, ROUNDS, "every round completed");
}, { timeoutMs: 120000 });

test("peakRss plateaus across the second half of the churn", (a) => {
  const instrumented = rss[0] > 16 * 1024 * 1024;
  if (instrumented)
    a.skip("peakRss plateau -- this is a sanitizer build (baseline " +
           rss[0] + " bytes), where a quarantining allocator makes RSS grow " +
           "whatever the code does");
  const { grewRss, ops } = M;
  check(grewRss <= 196608,
        "peakRss must PLATEAU across the second half of the churn -- it grew " +
        grewRss + " bytes over " + ops + " client lifecycles, which is " +
        Math.round(grewRss / ops) + " per lifecycle");
}, { dependsOn: [CHURN] });

test("the engine's own allocator does not accumulate", () => {
  const { grewEng } = M;
  check(grewEng <= 262144,
        "and the engine's own allocator must not be accumulating either " +
        "(it grew " + grewEng + "), which is what distinguishes a retained JS " +
        "object from a native leak");
}, { dependsOn: [CHURN] });

await t.run();
