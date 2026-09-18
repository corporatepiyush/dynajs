/* test_net_rss.js -- does memory PLATEAU across client lifecycles?
 *
 * The fd churn test asks the same question about descriptors; this asks it
 * about the heap. A leak of a few kilobytes per connection is invisible in any
 * single run and is exactly what a long-lived process cannot afford.
 *
 * TWO INSTRUMENTS, because neither alone is enough:
 *
 *   - `peakRss` is a per-process HIGH-WATER MARK, so it never falls. That makes
 *     it useless as a per-operation number and perfect for this question: if it
 *     stops rising while the work keeps repeating, nothing is accumulating.
 *   - the engine's own `mallocSize` is the CONTROL. These clients allocate with
 *     plain malloc, which the engine's counter cannot see, so a growing RSS
 *     with a flat engine counter points at the module or the allocator, and a
 *     growing engine counter points at retained JS objects. Without the control
 *     a rise says only "something grew".
 *
 * The comparison is between the SECOND half and the first, never against the
 * start: the first rounds include one-off warm-up -- arenas, the reactor, the
 * resolver's tables -- which is a constant, not a leak, and measuring from
 * round zero reports it as one.
 */
import { TCPServer, Redis, PostgreSQL } from "dyna:net";
import { memoryUsage } from "dyna:sys";

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

/* A payload big enough that a per-connection leak of the read buffer shows. */
const PAD = "x".repeat(4000);

const rbufs = new Map();
const rsrv = new TCPServer({ port: 0 });
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

const pbufs = new Map(), pstart = new Map();
const psrv = new TCPServer({ port: 0 });
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
    /* Answer the EXTENDED protocol too, or a parameterised query never settles
       and the round never completes -- which is a hung harness, not a leak. */
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

/* 24 was FLAKY, and not because of a leak: glibc takes one 128 KiB arena
   during the run, and at 24 rounds that step lands in the SECOND half and
   reads as 1820 bytes per lifecycle. Measured on Linux: 24 -> +131072,
   32/40/48 -> +0, 72 -> +0 over 216 lifecycles with the engine counter
   NEGATIVE. Bracketed rather than midpointed -- 24 fails, 32 passes, 40 is
   the margin. The docker leg later flaked AT 40 too (~50%): one more arena
   placement landing late. 80 was measured clean on both legs (Linux +0;
   macOS +131072/+81920, inside the bound below), so it is the default. */
const ROUNDS = parseInt(scriptArgs[1] || "80", 10);
const PER_ROUND = 6;
const rss = [], eng = [];
let round = 0;

function oneRound(done) {
  let left = PER_ROUND;
  for (let k = 0; k < PER_ROUND; k++) {
    const R = new Redis({ port: rsrv.port, host: "127.0.0.1" });
    const P = new PostgreSQL({ port: psrv.port, host: "127.0.0.1", user: "u" });
    Promise.all([
      R.command("GET", "k").catch(() => {}),
      R.pipeline([["GET", "a"], ["GET", "b"]]).catch(() => {}),
      P.query("SELECT 1").catch(() => {}),
      P.query("SELECT $1", ["v"]).catch(() => {}),
    ]).then(() => {
      R.close(); P.close();
      if (--left === 0) done();
    });
  }
}

function step() {
  if (round++ >= ROUNDS) { finish(); return; }
  oneRound(() => {
    const m = memoryUsage();
    rss.push(m.peakRss);
    eng.push(m.mallocSize);
    step();
  });
}

function finish() {
  rsrv.close(); psrv.close();
  const half = rss.length >> 1;
  const firstRss = rss[half - 1], lastRss = rss[rss.length - 1];
  const firstEng = eng[half - 1], lastEng = eng[eng.length - 1];
  const grewRss = lastRss - firstRss, grewEng = lastEng - firstEng;
  const ops = (rss.length - half) * PER_ROUND;

  print("rounds=" + rss.length + " x " + PER_ROUND + " clients");
  print("  peakRss   second half: " + firstRss + " -> " + lastRss +
        "  (+" + grewRss + " bytes over " + ops + " lifecycles)");
  print("  engine malloc         : " + firstEng + " -> " + lastEng +
        "  (+" + grewEng + ")");

  /* RSS IS NOT A VALID LEAK INSTRUMENT UNDER A SANITIZER. AddressSanitizer
     quarantines freed blocks rather than returning them, so RSS keeps climbing
     in proportion to allocation and a clean run is indistinguishable from a
     leaking one -- measured 243 KiB per lifecycle with the engine counter flat,
     which is the allocator saying so. Detect it from the baseline, which is
     ~4 MB clean and ~35 MB instrumented, and SKIP LOUDLY: a skip that prints
     nothing is how a suite quietly stops testing what it claims.
     The engine-allocator check below is valid in both builds and still runs. */
  const instrumented = rss[0] > 16 * 1024 * 1024;
  if (instrumented) {
    print("  SKIP: peakRss plateau -- this is a sanitizer build (baseline " +
          rss[0] + " bytes), where a quarantining allocator makes RSS grow " +
          "whatever the code does");
  } else {
    /* A high-water mark that has stopped rising is the plateau -- but HOW MUCH
       slack that needs is an ALLOCATOR property, not a code property, and
       tuning it on one platform is the same mistake as asserting a duration.
       Measured here, two runs each:

         platform   40 rounds        80 rounds        120 rounds
         Linux      +0               -                +0
         macOS      +98304 / +65536  +131072 / +81920 +98304 / +49152

       macOS does NOT settle with more rounds -- 3x the work gives identical
       growth -- which is exactly what says it is not a leak: a leak scales
       with the work and this does not. glibc plateaus at 0; Darwin's mark
       wanders under ~128 KiB whatever you do.

       So the bound sits above the observed ceiling, and the ENGINE COUNTER
       below is the instrument that actually discriminates a leak. A previous
       default of 40 rounds was calibrated on Linux alone and flaked here. */
    check(grewRss <= 196608,
          "peakRss must PLATEAU across the second half of the churn -- it grew " +
          grewRss + " bytes over " + ops + " client lifecycles, which is " +
          Math.round(grewRss / ops) + " per lifecycle");
  }
  check(grewEng <= 262144,
        "and the engine's own allocator must not be accumulating either " +
        "(it grew " + grewEng + "), which is what distinguishes a retained JS " +
        "object from a native leak");

  if (fails === 0) print("test_net_rss: all " + n + " checks passed");
  else {
    /* THROW. This printed "N FAILED of M" and returned 0, so make test-native
       reported success while the check was red -- a gate that cannot fail is
       not a gate. It had been failing on Linux, unnoticed, for exactly that
       reason. */
    print("test_net_rss: " + fails + " FAILED of " + n);
    throw new Error("test_net_rss: " + fails + " failures");
  }
}

step();
