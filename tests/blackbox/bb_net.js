// Parametric black-box contract test, generated from dynajs.d.ts (pre-split slice). Engine sources not consulted.
// Pure surface of dyna:net (addresses, Prefix, RateLimiter, Metrics, strict option bags)
// + at most one TCP loopback echo block and one UDP loopback block. No DNS, no TLS, no external hosts.
import {
  parseAddr, parsePrefix, canonical, isValid, compareAddr, contains, masked,
  isLoopback, isPrivate, isMulticast, isUnspecified, isLinkLocalUnicast,
  isGlobalUnicast, isLinkLocalMulticast,
  Prefix, RateLimiter, Metrics,
  Redis, PostgreSQL, DNSResolver, DNSServer, TCPServer, TCPProxy, UDPSocket,
  connectHappy, swallowedHandlerThrows,
} from "dyna:net";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
async function runRows(tableName, rows, fn) {
  for (let i = 0; i < rows.length; i++) {
    const row = rows[i];
    const label = tableName + "[" + i + "] " + row[0];
    try { await fn(row); }
    catch (e) { throw new Error("row failed — " + label + ": " + (e && e.message ? e.message : String(e))); }
  }
}
const bytesOf = (u8) => Array.from(u8);

await runRows("parseAddr", [
  ["ipv4", "127.0.0.1", (a) => {
    assertEq(a.is4, true, "is4"); assertEq(a.is6, false, "is6");
    assertDeepEq(bytesOf(a.bytes), [127, 0, 0, 1], "bytes");
    assertEq(a.string, "127.0.0.1", "string");
  }],
  ["ipv6-loopback", "::1", (a) => {
    assertEq(a.is6, true, "is6"); assertEq(a.is4, false, "is4");
    assertEq(a.bytes.length, 16, "16 bytes"); assertEq(a.string, "::1", "string");
  }],
  ["4-in-6-parses-as-v6", "::ffff:127.0.0.1", (a) => {
    assertEq(a.is6, true, "is6"); assertEq(a.is4, false, "is4");
    assertEq(a.string, "::ffff:127.0.0.1", "canonical string");
  }],
  ["v6-canonicalized", "2001:0DB8:0000::0001", (a) => assertEq(a.string, "2001:db8::1", "canonical string field")],
  // the driver evaluates parseAddr(input) BEFORE the check runs, so a refusal row
  // must catch around the value (d.ts: "throws TypeError on a malformed address")
  ["refusal-bad-octet", "999.1.1.1", (a) => a instanceof Error && a instanceof TypeError],
  ["refusal-not-an-address", "abc", (a) => a instanceof Error && a instanceof TypeError],
  ["refusal-empty", "", (a) => a instanceof Error && a instanceof TypeError],
], async ([, input, check]) => {
  let threw = null, v = null;
  try { v = parseAddr(input); } catch (e) { threw = e; }
  if (threw !== null) check(threw);
  else check(v);
});

await runRows("isValid", [
  ["valid-v4", "10.0.0.1", true],
  ["valid-v6", "fe80::1", true],
  ["invalid-octet", "999.1.1.1", false],
  ["invalid-garbage", "not-an-ip", false],
  ["empty", "", false],
], async ([, input, expected]) => assertEq(isValid(input), expected, "isValid never throws"));

await runRows("canonical", [
  ["all-zeros-v6", "0:0:0:0:0:0:0:1", "::1"],
  ["longest-run-zeroed", "2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1"],
  ["4-in-6-keeps-dotted", "::ffff:192.168.0.1", "::ffff:192.168.0.1"],
  ["v4-unchanged", "127.0.0.1", "127.0.0.1"],
], async ([, input, expected]) => assertEq(canonical(input), expected, "canonical"));

await runRows("parsePrefix", [
  // d.ts: parsePrefix only "format[s] addr canonically" — host-bit zeroing is the
  // separate masked() op ("The prefix's network address (host bits zeroed)").
  ["v4-addr-canonical", "10.1.2.3/8", { addr: "10.1.2.3", bits: 8 }],
  ["v6-canonicalized", "2001:db8:1:2::/32", { addr: "2001:db8:1:2::", bits: 32 }], // RFC 5952 compresses only the zero run; "2001:db8::" would drop 1:2
  ["refusal-no-slash", "10.0.0.0", (t) => assertThrows(t, "missing /bits refused", TypeError)],
  ["refusal-bad-bits", "10.0.0.0/33", (t) => assertThrows(t, "bits out of range refused", TypeError)],
  ["refusal-garbage", "nope/8", (t) => assertThrows(t, "garbage addr refused", TypeError)],
], async ([, input, expected]) => {
  if (typeof expected === "function") return expected(() => parsePrefix(input));
  const p = parsePrefix(input);
  assertEq(p.addr, expected.addr, "addr canonical"); assertEq(p.bits, expected.bits, "bits");
});

await runRows("masked/contains (free functions)", [
  ["masked-v4", () => masked("10.1.2.3/8"), (v) => assertEq(v, "10.0.0.0", "masked v4")],
  ["masked-v6", () => masked("2001:db8:ffff::/32"), (v) => assertEq(v, "2001:db8::", "masked v6")],
  ["masked-refusal", () => masked("10.0.0.0/33"), (t) => assertThrows(t, "malformed prefix refused", TypeError)],
  ["contains-hit", () => contains("10.0.0.0/8", "10.255.1.2"), (v) => assertEq(v, true, "inside")],
  ["contains-miss", () => contains("10.0.0.0/8", "11.0.0.1"), (v) => assertEq(v, false, "outside")],
  ["contains-v6", () => contains("2001:db8::/32", "2001:db8::1"), (v) => assertEq(v, true, "v6 inside")],
  ["contains-refusal-addr", () => contains("10.0.0.0/8", "nope"), (t) => assertThrows(t, "malformed addr refused", TypeError)],
  ["contains-refusal-prefix", () => contains("bad/99", "10.0.0.1"), (t) => assertThrows(t, "malformed prefix refused", TypeError)],
], async ([, call, check]) => {
  let threw = null, v = null;
  try { v = call(); } catch (e) { threw = e; }
  if (threw !== null) check(() => { throw threw; }); // assertThrows calls its thunk; rethrow the caught error
  else check(v);
});

await runRows("classification", [
  ["isLoopback-v4", isLoopback, "127.0.0.1", true],
  ["isLoopback-v6", isLoopback, "::1", true],
  ["isLoopback-no", isLoopback, "8.8.8.8", false],
  ["isPrivate-10", isPrivate, "10.0.0.1", true],
  ["isPrivate-192", isPrivate, "192.168.1.1", true],
  ["isPrivate-172.16", isPrivate, "172.16.0.1", true],
  ["isPrivate-172.32-outside", isPrivate, "172.32.0.1", false],
  ["isPrivate-public-no", isPrivate, "8.8.8.8", false],
  ["isPrivate-fc00", isPrivate, "fc00::1", true],
  ["isPrivate-fd00", isPrivate, "fd12::1", true],
  ["isPrivate-fe80-no", isPrivate, "fe80::1", false],
  ["isMulticast-v4", isMulticast, "224.0.0.1", true],
  ["isMulticast-v4-top", isMulticast, "239.9.9.9", true],
  ["isMulticast-v6", isMulticast, "ff02::1", true],
  ["isMulticast-no", isMulticast, "8.8.8.8", false],
  ["isUnspecified-v4", isUnspecified, "0.0.0.0", true],
  ["isUnspecified-v6", isUnspecified, "::", true],
  ["isUnspecified-no", isUnspecified, "127.0.0.1", false],
  ["isLinkLocalUnicast-v4", isLinkLocalUnicast, "169.254.9.9", true],
  ["isLinkLocalUnicast-v6", isLinkLocalUnicast, "fe80::1", true],
  ["isLinkLocalUnicast-no", isLinkLocalUnicast, "8.8.8.8", false],
  ["isLinkLocalMulticast-224.0.0.1", isLinkLocalMulticast, "224.0.0.1", true],
  ["isLinkLocalMulticast-224.0.0.251", isLinkLocalMulticast, "224.0.0.251", true],
  ["isLinkLocalMulticast-ff02", isLinkLocalMulticast, "ff02::fb", true],
  ["isLinkLocalMulticast-239-no", isLinkLocalMulticast, "239.1.1.1", false],
  ["isLinkLocalMulticast-ff0e-no", isLinkLocalMulticast, "ff0e::1", false],
  // per doc: "not loopback, multicast, link-local or unspecified" — RFC1918 is not excluded
  ["isGlobalUnicast-public", isGlobalUnicast, "8.8.8.8", true],
  ["isGlobalUnicast-private-per-doc", isGlobalUnicast, "192.168.0.1", true],
  ["isGlobalUnicast-loopback-no", isGlobalUnicast, "127.0.0.1", false],
  ["isGlobalUnicast-linklocal-no", isGlobalUnicast, "169.254.0.9", false],
  ["isGlobalUnicast-multicast-no", isGlobalUnicast, "224.0.0.5", false],
  ["isGlobalUnicast-unspecified-no", isGlobalUnicast, "0.0.0.0", false],
], async ([, fn, addr, expected]) => assertEq(fn(addr), expected, "classification"));

await runRows("compareAddr", [
  ["equal-v4", "127.0.0.1", "127.0.0.1", 0],
  ["less-v4", "10.0.0.1", "10.0.0.2", -1],
  ["greater-v4", "10.0.0.2", "10.0.0.1", 1],
  ["v4-before-v6", "8.8.8.8", "::1", -1],
  ["v6-after-v4", "::1", "8.8.8.8", 1],
], async ([, a, b, expected]) => assertEq(compareAddr(a, b), expected, "compareAddr total order"));

await runRows("Prefix", [
  ["v4-basics", () => new Prefix("10.0.0.0/8"), (p) => {
    assertEq(p.masked, "10.0.0.0", "masked"); assertEq(p.bits, 8, "bits"); assertEq(p.isIPv4, true, "isIPv4");
  }],
  ["v6-basics", () => new Prefix("2001:db8::/32"), (p) => {
    assertEq(p.masked, "2001:db8::", "masked"); assertEq(p.bits, 32, "bits"); assertEq(p.isIPv4, false, "isIPv4");
  }],
  ["contains", () => new Prefix("10.0.0.0/8"), (p) => {
    assertEq(p.contains("10.255.1.2"), true, "inside");
    assertEq(p.contains("11.0.0.1"), false, "outside");
    assertEq(p.contains("garbage"), false, "unparseable address is false, not an error");
  }],
  ["overlaps-hit", () => [new Prefix("10.0.0.0/8"), new Prefix("10.1.0.0/16")], ([a, b]) => assertEq(a.overlaps(b), true, "nested overlap")],
  ["overlaps-miss", () => [new Prefix("10.0.0.0/8"), new Prefix("11.0.0.0/8")], ([a, b]) => assertEq(a.overlaps(b), false, "disjoint")],
  ["families-never-overlap", () => [new Prefix("0.0.0.0/0"), new Prefix("::/0")], ([a, b]) => assertEq(a.overlaps(b), false, "v4 vs v6")],
  ["refusal-bad-bits", () => new Prefix("10.0.0.0/33"), (t) => assertThrows(t, "malformed cidr refused", TypeError)],
  ["refusal-garbage", () => new Prefix("nope"), (t) => assertThrows(t, "garbage cidr refused", TypeError)],
], async ([, build, check]) => {
  let threw = null, v = null;
  try { v = build(); } catch (e) { threw = e; }
  if (threw !== null) check(() => { throw threw; }); // assertThrows calls its thunk; rethrow the caught error
  else check(v);
});

await runRows("RateLimiter", [
  ["burst-defaults-to-one-second", { tokensPerSec: 5 }, (rl) => {
    assertEq(rl.stats.tokensPerSec, 5, "tokensPerSec echoed"); assertEq(rl.stats.burst, 5, "burst defaults to 1s of traffic");
  }],
  ["explicit-burst", { tokensPerSec: 5, burst: 10 }, (rl) => assertEq(rl.stats.burst, 10, "burst honored")],
  ["alias-equal-ok", { tokensPerSec: 5, refill: 5 }, (rl) => assertEq(rl.stats.tokensPerSec, 5, "equal alias pair accepted")],
  ["alias-only-ok", { refill: 7 }, (rl) => assertEq(rl.stats.tokensPerSec, 7, "refill alias works")],
  ["refusal-alias-conflict", () => new RateLimiter({ tokensPerSec: 5, refill: 9 }), (t) => assertThrows(t, "alias pair with different values throws")],
  ["refusal-missing-required", () => new RateLimiter({}), (t) => assertThrows(t, "tokensPerSec is required")],
  ["refusal-unknown-key", () => new RateLimiter({ tokensPerSec: 5, burzt: 1 }), (t) => assertThrows(t, "strict options bag", TypeError, /burzt/)],
  ["allow-consumes", { tokensPerSec: 3 }, (rl) => {
    assertEq(rl.allow("k"), true, "1st"); assertEq(rl.allow("k"), true, "2nd"); assertEq(rl.allow("k"), true, "3rd");
    assertEq(rl.allow("k"), false, "drained"); assert(rl.tokens("k") >= 0 && rl.tokens("k") < 1, "tokens near zero after drain, got " + rl.tokens("k"));
  }],
  ["cost-multi", { tokensPerSec: 10, burst: 10 }, (rl) => {
    assertEq(rl.allow("k", 10), true, "full-cost allowed"); assertEq(rl.allow("k"), false, "bucket empty after cost 10");
  }],
  ["refusal-cost-zero", { tokensPerSec: 5 }, (rl) => assertThrows(() => rl.allow("k", 0), "cost must be > 0")],
  ["refusal-cost-negative", { tokensPerSec: 5 }, (rl) => assertThrows(() => rl.allow("k", -1), "cost must be > 0")],
  ["reset-key-refills", { tokensPerSec: 1, burst: 1 }, (rl) => {
    assertEq(rl.allow("k"), true, "consumed");
    rl.reset("k");
    assertEq(rl.allow("k"), true, "allowed again after reset(key)");
  }],
  ["reset-all", { tokensPerSec: 1, burst: 1 }, (rl) => {
    assertEq(rl.allow("a"), true, "a consumed"); assertEq(rl.allow("b"), true, "b consumed");
    rl.reset();
    assertEq(rl.allow("a"), true, "a after reset()"); assertEq(rl.allow("b"), true, "b after reset()");
  }],
  ["stats-counters", { tokensPerSec: 5 }, (rl) => {
    rl.allow("k"); rl.allow("k"); rl.allow("k"); rl.allow("k"); rl.allow("k");
    rl.allow("k"); rl.allow("k");
    assertEq(rl.stats.allowed, 5, "allowed tally"); assertEq(rl.stats.denied, 2, "denied tally");
    assertEq(rl.stats.grew, 0, "no growth without grow");
    assert(typeof rl.stats.slots === "number" && rl.stats.slots > 0, "table geometry present");
    assert(typeof rl.stats.live === "number", "live present");
  }],
  ["grow-accepted", { tokensPerSec: 2, grow: true }, (rl) => {
    assert(typeof rl.stats.grew === "number", "grew is a number");
  }],
  // Regression (rate-limit bypass): a key that collides with another key's table slot
  // must probe for its own entry instead of resetting the occupant to a full burst
  // (d.ts: "one key's traffic never resets another key's budget"). The FNV-1a mirror
  // constructs guaranteed colliding aliases -- the offline-searchable attack.
  ["colliding-key-never-refreshes-victim", { tokensPerSec: 1, burst: 1, slots: 8 }, (rl) => {
    const M = 0xffffffffffffffffn;
    const fnv = (s) => { let h = 1469598103934665603n; for (let i = 0; i < s.length; i++) h = ((h ^ BigInt(s.charCodeAt(i))) * 1099511628211n) & M; return h; };
    const home = (s) => Number(fnv(s) & 7n);
    const colliders = [];
    for (let i = 0; colliders.length < 3; i++) {
      const c = "alias" + i;
      if (fnv(c) !== fnv("vict") && home(c) === home("vict")) colliders.push(c);
    }
    assert(colliders.length === 3, "constructed colliding aliases");
    assertEq(rl.allow("vict"), true, "victim first");
    assertEq(rl.allow("vict"), false, "victim drained");
    for (const c of colliders) assertEq(rl.allow(c), true, "collider " + c + " gets its own budget");
    assertEq(rl.allow("vict"), false, "victim budget NOT reset by colliding traffic");
    assert(rl.tokens("vict") < 1, "victim tokens still drained, got " + rl.tokens("vict"));
  }],
  // d.ts: "past which a new key evicts the least-recently-used entry instead of
  // sharing a slot" -- a full table keeps serving new keys at bounded capacity.
  ["full-table-evicts-not-refuses", { tokensPerSec: 1, burst: 1000, slots: 8 }, (rl) => {
    for (let i = 0; i < 8; i++) assertEq(rl.allow("k" + i, 1000), true, "k" + i + " full drain");
    assertEq(rl.stats.live, 8, "table holds 8");
    assertEq(rl.allow("k8"), true, "9th key still served");
    assertEq(rl.stats.live, 8, "capacity stays bounded");
    let fresh = 0;
    for (let i = 0; i < 8; i++) if (rl.allow("k" + i, 1000)) fresh++;
    assertEq(fresh, 1, "exactly one prior key was evicted");
  }],
  // d.ts: grow "opts into doubling past a 3/4 load factor" -- rehash must carry every
  // live budget over, not drop entries back to a full burst.
  ["grow-rehash-preserves-budgets", { tokensPerSec: 1, burst: 1000, slots: 8, grow: true }, (rl) => {
    assertEq(rl.allow("a", 1000), true, "a drained fully");
    assertEq(rl.allow("a"), false, "a denied while empty");
    for (let i = 0; i < 60; i++) rl.allow("filler" + i);
    assert(rl.stats.grew > 0, "table grew, got " + rl.stats.grew);
    assert(rl.tokens("a") < 1000, "a not refilled to burst by rehash, got " + rl.tokens("a"));
    assertEq(rl.allow("a"), false, "a still denied after rehash");
  }],
  // d.ts: reset(key) "Clears one key" -- only that key's budget is affected.
  ["reset-key-keeps-neighbors", { tokensPerSec: 1, burst: 1 }, (rl) => {
    assertEq(rl.allow("a"), true, "a first"); assertEq(rl.allow("b"), true, "b first");
    assertEq(rl.allow("a"), false, "a drained"); assertEq(rl.allow("b"), false, "b drained");
    rl.reset("a");
    assertEq(rl.allow("a"), true, "a refilled by reset");
    assertEq(rl.allow("b"), false, "b still drained after reset(a)");
  }],
], async ([, optsOrBuild, check]) => {
  let threw = null, v = null;
  try { v = typeof optsOrBuild === "function" ? optsOrBuild() : new RateLimiter(optsOrBuild); } catch (e) { threw = e; }
  if (threw !== null) check(() => { throw threw; }); // assertThrows calls its thunk; rethrow the caught error
  else check(v);
});

{ // Metrics is a fixed registry with a Prometheus text scrape
  Metrics.reset();
  await runRows("Metrics", [
    ["counter-increments", null, () => { Metrics.counter("bb_m_c", 1); Metrics.counter("bb_m_c", 2); assert(Metrics.scrape().includes("bb_m_c"), "counter name in scrape"); }],
    ["counter-labels", null, () => { Metrics.counter("bb_m_cl", 1, { a: "b" }); const s = Metrics.scrape(); assert(s.includes("bb_m_cl"), "name"); assert(s.includes('a="b"'), "labels, got |" + s + "|"); }],
    ["counter-default-increment", null, () => { Metrics.counter("bb_m_c2"); assert(Metrics.scrape().includes("bb_m_c2"), "name present"); }],
    ["refusal-negative-increment", null, () => assertThrows(() => Metrics.counter("bb_m_bad", -1), "negative increment refused")],
    ["refusal-nan-increment", null, () => assertThrows(() => Metrics.counter("bb_m_bad", NaN), "NaN increment refused")],
    ["gauge-sets", null, () => { Metrics.gauge("bb_m_g", 3.5); assert(Metrics.scrape().includes("bb_m_g"), "gauge name in scrape"); }],
    ["histogram-default-buckets", null, () => { Metrics.histogram("bb_m_h", 0.05); assert(Metrics.scrape().includes("bb_m_h"), "histogram name in scrape"); }],
    ["histogram-custom-buckets", null, () => { Metrics.histogram("bb_m_h2", 0.05, undefined, { buckets: [0.005, 0.05, 0.5, 1] }); assert(Metrics.scrape().includes("bb_m_h2"), "name present"); }],
    // d.ts pins the error CLASS only for the strict opts bag ("an unknown key throws
    // a TypeError"); the bucket-edge refusals are RangeErrors (counter's documented
    // refusal family), so the rows check the message, not the class.
    ["refusal-buckets-empty", null, () => assertThrows(() => Metrics.histogram("bb_m_bad", 0.1, undefined, { buckets: [] }), "0 edges refused")],
    ["refusal-buckets-seven", null, () => assertThrows(() => Metrics.histogram("bb_m_bad", 0.1, undefined, { buckets: [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7] }), "7 edges refused")],
    ["refusal-buckets-decreasing", null, () => assertThrows(() => Metrics.histogram("bb_m_bad", 0.1, undefined, { buckets: [0.5, 0.1] }), "not increasing refused")],
    ["refusal-buckets-equal", null, () => assertThrows(() => Metrics.histogram("bb_m_bad", 0.1, undefined, { buckets: [1, 1] }), "strictly increasing refused")],
    ["refusal-buckets-negative", null, () => assertThrows(() => Metrics.histogram("bb_m_bad", 0.1, undefined, { buckets: [-1, 1] }), "negative edge refused")],
    ["refusal-buckets-nonfinite", null, () => assertThrows(() => Metrics.histogram("bb_m_bad", 0.1, undefined, { buckets: [Infinity] }), "non-finite edge refused")],
    ["refusal-unknown-opts-key", null, () => assertThrows(() => Metrics.histogram("bb_m_bad", 0.1, undefined, { bucketz: [0.1] }), "strict opts bag", TypeError, /bucketz/)],
    ["edges-fixed-once-series-exists", null, () => { Metrics.histogram("bb_m_fix", 0.05, undefined, { buckets: [0.01, 0.1] }); Metrics.histogram("bb_m_fix", 0.05); assert(Metrics.scrape().includes("bb_m_fix"), "later calls without opts are fine"); }],
    ["reset-empties", null, () => { Metrics.reset(); assertEq(Metrics.scrape().includes("bb_m_c"), false, "registry emptied"); }],
  ], async ([, , check]) => check());
}

await runRows("strict option bags (constructed but never connected)", [
  ["Redis-unknown-key", () => new Redis({ portz: 1 }), (t) => assertThrows(t, "strict bag", TypeError)],
  ["Redis-documented-message", () => new Redis({ portz: 1 }), (t) => assertThrows(t, "doc names key and valid set", TypeError, /unknown option "portz"/)],
  ["PostgreSQL-unknown-key", () => new PostgreSQL({ portz: 1 }), (t) => assertThrows(t, "strict bag", TypeError, /portz/)],
  ["DNSResolver-unknown-key", () => new DNSResolver({ srvr: "x" }), (t) => assertThrows(t, "strict bag", TypeError, /srvr/)],
  ["DNSServer-unknown-key", () => new DNSServer({ portz: 5353 }), (t) => assertThrows(t, "strict bag", TypeError, /portz/)],
  ["TCPServer-unknown-key", () => new TCPServer({ portz: 0 }), (t) => assertThrows(t, "strict bag", TypeError, /portz/)],
  ["TCPProxy-unknown-key", () => new TCPProxy({ port: 0, upstream: { port: 1 }, bogus: 1 }), (t) => assertThrows(t, "strict bag", TypeError, /bogus/)],
  ["connect-handlers-strict", () => TCPServer.connect({ host: "127.0.0.1", port: 1 }, { datum() {} }), (t) => assertThrows(t, "handler bag strict", TypeError, /datum/)],
  ["connectHappy-port-zero", () => connectHappy("127.0.0.1", 0), (t) => assertThrows(t, "port must be 1-65535")],
  ["connectHappy-port-too-big", () => connectHappy("127.0.0.1", 70000), (t) => assertThrows(t, "port must be 1-65535")],
  ["null-bag-counts-as-absent", () => new TCPServer(null), (srv) => { assertEq(srv.closed, false, "built with null bag"); srv.close(); assertEq(srv.closed, true, "closed"); }],
  ["primitive-bag-counts-as-absent", () => new TCPServer(42), (srv) => { srv.close(); }],
  ["inherited-keys-invisible", () => { const bag = Object.create({ portz: 1 }); bag.tokensPerSec = 5; return new RateLimiter(bag); }, (rl) => { assertEq(rl.stats.tokensPerSec, 5, "inherited key not seen, no throw"); }],
  ["symbol-keys-invisible", () => { const bag = { tokensPerSec: 5 }; bag[Symbol("portz")] = 1; return new RateLimiter(bag); }, (rl) => { assertEq(rl.stats.tokensPerSec, 5, "symbol key not seen, no throw"); }],
], async ([, build, check]) => {
  let threw = null, v = null;
  try { v = build(); } catch (e) { threw = e; }
  if (threw !== null) check(() => { throw threw; }); // assertThrows calls its thunk; rethrow the caught error
  else check(v);
});

await runRows("swallowedHandlerThrows", [
  ["returns-number", null, () => assert(typeof swallowedHandlerThrows() === "number", "counter is a number")],
  ["monotonic-non-decreasing", null, () => { const a = swallowedHandlerThrows(); const b = swallowedHandlerThrows(); assert(b >= a, "monotonic"); }],
], async ([, , check]) => check());

/* ------------------------------------------------------------------ *
 *  ONE TCP loopback echo block: ephemeral port via port: 0, connect,
 *  write, read back, close in finally. Content asserted, never timing.
 * ------------------------------------------------------------------ */
{
  const srv = new TCPServer({ port: 0, maxConnections: 4 });
  let serverSaw = null;
  srv.start({
    data(conn, bytes) { serverSaw = bytesOf(bytes); conn.write(bytes); },
  });
  assert(srv.port > 0, "ephemeral TCP port resolved, got " + srv.port);

  let clientGot = null;
  const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port, connectTimeoutMs: 1000 }, {
    connect(conn) {
      assert(typeof conn.bufferedAmount === "number", "bufferedAmount is a number");
      // BytesInput = string | ByteView (d.ts L74): anything else is a
      // TypeError, never a silent "[object Object]" payload.
      assertThrows(() => conn.write({}), "write({}) refused as a TypeError", TypeError, /string or a byte view/);
      assertThrows(() => conn.write(42), "write(42) refused as a TypeError", TypeError, /string or a byte view/);
      conn.write("ping-echo");
    },
    data(conn, bytes) { clientGot = new TextDecoder().decode(bytes); conn.close(); },
  });
  try {
    const t0 = Date.now();
    while (clientGot === null && Date.now() - t0 < 1500) await sleep(5);
    assert(clientGot !== null, "tcp echo: no data came back within the deadline");
    assertEq(clientGot, "ping-echo", "tcp echo round trip");
    assertDeepEq(serverSaw, bytesOf(new TextEncoder().encode("ping-echo")), "server saw the sent bytes");
  } finally {
    cli.dispose();
    srv.dispose();
  }
}

/* ------------------------------------------------------------------ *
 *  ONE UDP loopback block: two sockets on 127.0.0.1 port 0, one send,
 *  one receive. No DNS, no external hosts.
 * ------------------------------------------------------------------ */
{
  const rx = new UDPSocket({ port: 0, host: "127.0.0.1" });
  const got = [];
  rx.start({
    message(data, from) { got.push({ bytes: bytesOf(data), from }); },
  });
  assert(rx.port > 0, "ephemeral UDP port resolved, got " + rx.port);

  const tx = new UDPSocket({ port: 0, host: "127.0.0.1" });
  try {
    // BytesInput = string | ByteView (d.ts L74): non-buffer non-strings are
    // refused, never serialized as "[object Object]".
    assertThrows(() => tx.send({}, "127.0.0.1", rx.port), "send({}) refused as a TypeError", TypeError, /string or a byte view/);
    const sentText = tx.send("hello-udp", "127.0.0.1", rx.port);
    const sentBytes = tx.send(new Uint8Array([1, 2, 3]), "127.0.0.1", rx.port);
    await runRows("UDP loopback", [
      ["send-returns-bytes-string", null, () => assertEq(sentText, 9, "bytes sent for a 9-char string")],
      ["send-returns-bytes-u8", null, () => assertEq(sentBytes, 3, "bytes sent for 3-byte payload")],
    ], async ([, , check]) => check());
    // poll with sleeps so the event loop drains between ticks
    const t0 = Date.now();
    while (got.length < 2 && Date.now() - t0 < 1500) await sleep(5);
    assert(got.length >= 2, "udp: only " + got.length + " of 2 datagrams arrived within the deadline");
    const text = got.find((g) => g.bytes.length === 9);
    const bin = got.find((g) => g.bytes.length === 3);
    assert(text !== undefined, "text datagram received");
    assertDeepEq(text.bytes, bytesOf(new TextEncoder().encode("hello-udp")), "text payload intact"); // bytesOf both sides: JSON deep-compare cannot match Array vs Uint8Array
    assertEq(text.from.address, "127.0.0.1", "source address");
    assertEq(text.from.port, tx.port, "source port is the sender's bound port");
    assert(bin !== undefined, "binary datagram received");
    assertDeepEq(bin.bytes, [1, 2, 3], "binary payload intact");
  } finally {
    tx.close();
    rx.close();
  }
}

/* ------------------------------------------------------------------ *
 *  dyna:http re-export identity + a net.SQLite slice on a temp db
 *  (both local, no network).
 * ------------------------------------------------------------------ */
{ // d.ts:2304-2305: "Same class as dyna:http.HTTPClient; re-exported here."
  const netMod = await import("dyna:net");
  const httpMod = await import("dyna:http");
  assertEq(typeof netMod.HTTPClient, "function", "HTTPClient is a constructor value of dyna:net");
  assertEq(netMod.HTTPClient, httpMod.HTTPClient, "net.HTTPClient IS dyna:http.HTTPClient (same class object)");
  assertEq(httpMod.HTTPClient, netMod.HTTPClient, "the identity holds from the dyna:http side too");
}

{ // d.ts:2636-2654: "A SQLite database handle; every value is bound, never
  // interpolated." Audit item 8 slice: temp-db exec/query/lastInsertRowId.
  const netMod = await import("dyna:net");
  const fileMod = await import("dyna:file");
  const dir = fileMod.makeTempDir("bbnet-sqlite-");
  try {
    const SQLite = netMod.SQLite;
    assertEq(typeof SQLite, "function", "SQLite is a constructor value of dyna:net");
    const dbPath = String(new fileMod.Path(dir, "t.db"));
    const db = new SQLite(dbPath);
    assert(typeof db.version === "string" && db.version.length > 0,
        "sqlite [the linked library's version is a non-empty string]");
    assertEq(db.closed, false, "sqlite [fresh handle is open]");
    assertEq(db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL)"), 0,
        "sqlite [exec returns rows changed — DDL changes 0]");
    assertEq(db.exec("INSERT INTO t (name, score) VALUES (?, ?)", ["alice", 1.5]), 1,
        "sqlite [exec returns 1 for one inserted row]");
    const evil = "'; DROP TABLE t; --";
    assertEq(db.exec("INSERT INTO t (name, score) VALUES (?, ?)", [evil, 2.0]), 1,
        "sqlite [the injection string binds as DATA]");
    assertEq(db.lastInsertRowId, 2, "sqlite [lastInsertRowId after two inserts]");
    const all = db.query("SELECT id, name, score FROM t ORDER BY id");
    assertEq(all.length, 2, "sqlite [both rows survive — the table was not dropped]");
    assertEq(all[0].name, "alice", "sqlite [row 0 name]");
    assertEq(all[0].score, 1.5, "sqlite [REAL round trip]");
    assertEq(all[1].name, evil, "sqlite [injection string round-trips as data]");
    assertEq(db.query("SELECT name FROM t WHERE name = ?", ["alice"]).length, 1,
        "sqlite [parameterised WHERE matches exactly one row]");
    db.close();
    assertEq(db.closed, true, "sqlite [close() marks closed]");
    // the temp-db FILE persisted: reopen and count
    const db2 = new SQLite(dbPath);
    assertEq(db2.query("SELECT COUNT(*) AS c FROM t")[0].c, 2, "sqlite [the temp db file persisted its rows]");
    db2.close();
  } finally {
    fileMod.removeAll(dir);
  }
}

print("bb_net: all tests passed (" + n + " assertions)");
