// covers: dyna:postgres.PostgreSQL
// Split from tests/net/test_net_rss.js (one module per suite).
import { TCPServer } from "dyna:net";
import { PostgreSQL } from "dyna:postgres";
import { memoryUsage } from "dyna:sys";

import { suite } from "../lib/kit.js";

// The mock PostgreSQL server starts in the kit's before hook (never
// under --list); the churn runs once and the two plateau checks read it.
const t = suite("test_postgres_postgre_sql_rss");
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

let psrv = null;
t.before(() => {
const pbufs = new Map(), pstart = new Map();
psrv = new TCPServer({ port: 0 });
psrv.start({ data: (c, b) => {
  pbufs.set(c, (pbufs.get(c) || "") + latin1(b));
  let out = "";
  for (;;) {
    const s = pbufs.get(c);
    if (!pstart.get(c)) {
      if (s.length < 8) break;
      const len = rd32(s, 0);
      if (s.length < len) break;
      pbufs.set(c, s.slice(len));
      pstart.set(c, true);
      out += msg("R", i32(0)) + msg("K", i32(5) + "abcd") + msg("Z", "I");
      continue;
    }
    if (s.length < 5) break;
    const len = rd32(s, 1);
    if (s.length < len + 1) break;
    const type = s[0];
    pbufs.set(c, s.slice(len + 1));
    if (type === "P") { out += msg("1", ""); continue; }
    if (type === "B") { out += msg("2", ""); continue; }
    if (type === "D") { out += msg("n", ""); continue; }
    if (type === "E" || type === "Q") {
      out += msg("T", i16(1) + cstr("a") + i32(0) + i16(0) + i32(25) +
                      i16(0xffff) + i32(0xffffffff) + i16(0)) +
             msg("D", i16(1) + i32(PAD.length) + PAD) +
             msg("C", cstr("SELECT 1"));
      if (type === "Q") out += msg("Z", "I");
      continue;
    }
    if (type === "S") { out += msg("Z", "I"); continue; }
    continue;
  }
  if (out) c.write(bytes(out));
}, close: (c) => { pbufs.delete(c); pstart.delete(c); } });
});
t.after(() => { if (psrv) psrv.close(); });

const ROUNDS = parseInt(scriptArgs[1] || "80", 10);
const PER_ROUND = 6;
const rss = [], eng = [];
let round = 0;

function oneRound(done) {
  let left = PER_ROUND;
  for (let k = 0; k < PER_ROUND; k++) {
    const P = new PostgreSQL({ port: psrv.port, host: "127.0.0.1", user: "u" });
    Promise.all([
      P.query("SELECT 1").catch(() => {}),
      P.query("SELECT $1", ["v"]).catch(() => {}),
    ]).then(() => {
      P.close();
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
