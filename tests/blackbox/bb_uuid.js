// Parametric black-box contract test for dyna:uuid, generated from dynajs.d.ts lines 5955-6012. Engine sources not consulted.
import * as uuid from "dyna:uuid";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertClose(actual, expected, eps, msg) { n++; if (!(Math.abs(actual - expected) <= eps)) throw new Error("assertion failed (close): " + msg + " — got |" + actual + "| expected |" + expected + "±" + eps + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

const V4_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/; // version nibble '4', variant nibble [89ab]
const V7_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/; // version nibble '7', same variant
const ULID_RE = /^[0-9A-HJKMNP-TV-Z]{26}$/; // Crockford base32 excludes I, L, O, U

// ============================ constants (d.ts L6003-6011) ============================
// "The all-zero UUID" / "The all-ones UUID" / "Predefined RFC 4122 name namespaces."
{
  const cases = [
    ["NIL", uuid.NIL, "00000000-0000-0000-0000-000000000000"],
    ["MAX", uuid.MAX, "ffffffff-ffff-ffff-ffff-ffffffffffff"],
  ];
  for (const [label, got, expected] of cases) assertEq(got, expected, label);
  const ns = [uuid.NAMESPACE_DNS, uuid.NAMESPACE_URL, uuid.NAMESPACE_OID, uuid.NAMESPACE_X500];
  assert(new Set(ns).size === 4, "the four RFC 4122 namespaces are distinct");
  for (const nsv of ns) {
    assertEq(uuid.validate(nsv), true, "namespace " + nsv + " is itself a valid UUID");
    assertEq(uuid.fromBytes(uuid.bytes(nsv)), nsv, "namespace " + nsv + " is in canonical form");
  }
}

// ============================ constants: exact namespace tail (d.ts L6011-6015) ============================
// "Predefined RFC 4122 name namespaces." — RFC 4122 Appendix C pins the exact strings, and the
// name-based UUID algorithm (d.ts L5977-5980: "MD5-based" / "SHA-1-based name UUID") turns each
// namespace into a cross-check: the vectors below are the RFC algorithm (MD5/SHA-1 over
// namespace-bytes + name, version/variant bits set) computed offline.
{
  const cases = [
    ["NAMESPACE_DNS", uuid.NAMESPACE_DNS, "6ba7b810-9dad-11d1-80b4-00c04fd430c8"],
    ["NAMESPACE_URL", uuid.NAMESPACE_URL, "6ba7b811-9dad-11d1-80b4-00c04fd430c8"],
    ["NAMESPACE_OID", uuid.NAMESPACE_OID, "6ba7b812-9dad-11d1-80b4-00c04fd430c8"],
    ["NAMESPACE_X500", uuid.NAMESPACE_X500, "6ba7b814-9dad-11d1-80b4-00c04fd430c8"],
  ];
  for (const [label, got, expected] of cases) assertEq(got, expected, label);
  const nameVectors = [
    ["v3 over NAMESPACE_DNS", uuid.v3, "6fa459ea-ee8a-3ca4-894e-db77e160355e"],
    ["v5 over NAMESPACE_DNS", uuid.v5, "886313e1-3b8a-5372-9b90-0c9aee199e5d"],
    ["v5 over NAMESPACE_URL", uuid.v5, "4fd35a71-71ef-5a55-a9d9-aa75c889a6d0"],
    ["v5 over NAMESPACE_OID", uuid.v5, "67448b45-6d15-536b-bef7-8a78c3d10ac6"],
    ["v5 over NAMESPACE_X500", uuid.v5, "1a09ab7e-5f76-53b6-b866-3b565a53dc51"],
  ];
  for (const [label, fn, expected] of nameVectors) {
    const nsName = label.includes("NAMESPACE_URL") ? uuid.NAMESPACE_URL
      : label.includes("NAMESPACE_OID") ? uuid.NAMESPACE_OID
      : label.includes("NAMESPACE_X500") ? uuid.NAMESPACE_X500 : uuid.NAMESPACE_DNS;
    const name = label.includes("v3") ? "python.org"
      : label.includes("URL") ? "https://example.com"
      : label.includes("OID") ? "1.2.3.4"
      : label.includes("X500") ? "cn=Test" : "python.org";
    assertEq(fn(nsName, name), expected, label + " matches the RFC 4122 algorithm vector");
  }
  // the namespace string and its 16 raw bytes are interchangeable namespace arguments
  assertEq(uuid.v5(uuid.NAMESPACE_DNS, "python.org"), uuid.v5(uuid.bytes(uuid.NAMESPACE_DNS), "python.org"),
           "namespace accepts its canonical string or raw bytes identically");
}

// ============================ v4 (d.ts L5956-5959) ============================
// "Random version-4 UUID. Unknown option keys are refused (TypeError)." / "{as: 'bytes'}: the 16 bytes a v4 string would encode"
{
  const cases = [
    ["v4() default door", () => uuid.v4(), "string"],
    ["v4({as:'string'}) explicit door", () => uuid.v4({ as: "string" }), "string"],
  ];
  for (const [label, fn] of cases) {
    const s = fn();
    assert(typeof s === "string" && V4_RE.test(s), label + " matches the v4 shape (lowercase, version 4, variant [89ab])");
  }
  const b = uuid.v4({ as: "bytes" });
  assert(b instanceof Uint8Array && b.length === 16, "v4 bytes door is 16 bytes");
  assertEq(b[6] >> 4, 4, "v4 bytes: version nibble of byte 6 is 4");
  assertEq(b[8] >> 6, 2, "v4 bytes: variant bits of byte 8 are 10");
  // format/parse round-trip through the documented bytes door
  const s = uuid.v4();
  assertEq(uuid.parse(s), s, "v4 output is already canonical (parse is identity)");
  assertEq(uuid.fromBytes(uuid.bytes(s)), s, "fromBytes(bytes(s)) round-trips a v4");
  assertThrows(() => uuid.v4({ as: "string", bogus: 1 }), "v4 unknown option key refused", TypeError);
}

// ============================ v7 (d.ts L5960-5963) ============================
// "Time-ordered version-7 UUID ... ids generated later in the process sort after earlier ones."
{
  const s = uuid.v7();
  assert(V7_RE.test(s), "v7() matches the v7 shape (version nibble 7, variant [89ab])");
  const b = uuid.v7({ as: "bytes" });
  assert(b instanceof Uint8Array && b.length === 16, "v7 bytes door is 16 bytes");
  assertEq(b[6] >> 4, 7, "v7 bytes: version nibble of byte 6 is 7");
  assertEq(b[8] >> 6, 2, "v7 bytes: variant bits of byte 8 are 10");
  assertEq(uuid.version(s), 7, "version(v7()) = 7");
  // monotonic/sortable: consecutive generation is strictly ascending (same-ms counter or later ms)
  const ids = [];
  for (let i = 0; i < 30; i++) ids.push(uuid.v7());
  for (let i = 1; i < ids.length; i++) assert(ids[i] > ids[i - 1], "v7 id " + i + " sorts after its predecessor");
}

// ============================ v8 (d.ts L5964-5968) ============================
// "The caller supplies ALL 16 bytes; only the version nibble (byte 6 high <- 1000) and the variant bits
//  (byte 8 top two <- 10) are overwritten, everything else survives. The fill view is copied, never
//  mutated; wrong length is a RangeError."
{
  const fill = new Uint8Array(16);
  for (let i = 0; i < 16; i++) fill[i] = i;
  const s = uuid.v8({ fill });
  assert(typeof s === "string", "v8 returns a string");
  const out = uuid.bytes(s);
  assertEq(out.length, 16, "v8 output parses to 16 bytes");
  assertEq(out[6], 0x86, "v8 byte 6: high nibble <- 1000 over fill value 6 (0x86)");
  assertEq(out[8], 0x88, "v8 byte 8: top two bits <- 10 over fill value 8 (0x88)");
  for (let i = 0; i < 16; i++) {
    if (i !== 6 && i !== 8) assertEq(out[i], i, "v8 byte " + i + " survives untouched");
  }
  assert(eqArr([...fill], [...Array(16).keys()]), "v8 never mutates the fill view");
  assertEq(uuid.version(s), 8, "version(v8 string) = 8");
  assertEq(uuid.variant(s), "RFC4122", "v8 variant bits 10 -> RFC4122");
  // ByteView door: DataView fill is accepted (ByteView)
  const buf = new ArrayBuffer(16);
  const dv = new DataView(buf);
  for (let i = 0; i < 16; i++) dv.setUint8(i, 0x11);
  const s2 = uuid.v8({ fill: dv });
  assertEq(uuid.version(s2), 8, "v8 accepts a DataView fill (ByteView door)");
  const refusals = [
    ["v8 fill of 15 bytes", () => uuid.v8({ fill: new Uint8Array(15) }), RangeError],
    ["v8 fill of 17 bytes", () => uuid.v8({ fill: new Uint8Array(17) }), RangeError],
  ];
  for (const [label, fn, T] of refusals) assertThrows(fn, label, T);
}

// ============================ compare (d.ts L5969-5972) ============================
// "-1 | 0 | 1 comparing the canonical 16-byte forms (memcmp semantics) ... TypeError on a malformed or non-string argument."
{
  const cases = [
    ["NIL vs NIL", uuid.NIL, uuid.NIL, 0],
    ["NIL vs MAX", uuid.NIL, uuid.MAX, -1],
    ["MAX vs NIL", uuid.MAX, uuid.NIL, 1],
    ["MAX vs MAX", uuid.MAX, uuid.MAX, 0],
  ];
  for (const [label, a, b, expected] of cases) assertEq(uuid.compare(a, b), expected, label);
  const p = uuid.v4();
  assertEq(uuid.compare(p, uuid.parse(p)), 0, "compare is form-agnostic (canonical vs parsed)");
  assertEq(uuid.compare(p, p), 0, "compare of identical ids is 0");
  const refusals = [
    ["compare malformed string", () => uuid.compare("not-a-uuid", uuid.NIL), TypeError],
    ["compare non-string arg", () => uuid.compare(123, uuid.NIL), TypeError],
  ];
  for (const [label, fn, T] of refusals) assertThrows(fn, label, T);
}

// ============================ v3 / v5 (d.ts L5973-5976) ============================
// "MD5-based name UUID" / "SHA-1-based name UUID" — version nibbles 3 and 5, deterministic in (namespace, name).
{
  const cases = [
    ["v3 version nibble", (ns, name) => uuid.v3(ns, name), 3],
    ["v5 version nibble", (ns, name) => uuid.v5(ns, name), 5],
  ];
  for (const [label, fn, expected] of cases) {
    const id = fn(uuid.NAMESPACE_DNS, "example.com");
    assertEq(uuid.version(id), expected, label);
    assertEq(uuid.variant(id), "RFC4122", label + " carries RFC4122 variant bits");
    assertEq(fn(uuid.NAMESPACE_DNS, "example.com"), id, label + " is deterministic in (namespace, name)");
  }
  const v5a = uuid.v5(uuid.NAMESPACE_DNS, "a");
  const v5b = uuid.v5(uuid.NAMESPACE_DNS, "b");
  assert(v5a !== v5b, "v5 differs for different names");
  assert(uuid.v5(uuid.NAMESPACE_URL, "a") !== uuid.v5(uuid.NAMESPACE_DNS, "a"), "v5 differs for different namespaces");
  // parity: the namespace string door and its parsed-bytes door name the same 16 bytes
  assertEq(uuid.v5(uuid.NAMESPACE_DNS, "example.com"), uuid.v5(uuid.bytes(uuid.NAMESPACE_DNS), "example.com"), "v5 namespace string door === bytes door");
  assertEq(uuid.v3(uuid.NAMESPACE_DNS, "example.com"), uuid.v3(uuid.bytes(uuid.NAMESPACE_DNS), "example.com"), "v3 namespace string door === bytes door");
}

// ============================ parse / validate (d.ts L5977-5980) ============================
// "Parses any accepted form and returns the canonical lowercase string." / "True iff the argument is a string in an accepted form."
{
  const cases = [
    ["parse NIL", uuid.NIL, uuid.NIL],
    ["parse MAX", uuid.MAX, uuid.MAX],
    ["parse uppercase canonical -> lowercase", uuid.MAX.toUpperCase(), uuid.MAX],
    ["parse v4 is identity", (() => { const x = uuid.v4(); return [x, x]; })()],
  ];
  for (const [label, input, expected] of cases) {
    const arg = Array.isArray(input) ? input[0] : input;
    const exp = Array.isArray(input) ? input[1] : expected;
    assertEq(uuid.parse(arg), exp, label);
  }
  const okCases = [
    ["validate v4", uuid.v4(), true],
    ["validate NIL", uuid.NIL, true],
    ["validate MAX", uuid.MAX, true],
    ["validate namespace", uuid.NAMESPACE_URL, true],
  ];
  for (const [label, v, expected] of okCases) assertEq(uuid.validate(v), expected, label);
  const badCases = [
    ["validate garbage string", "not-a-uuid", false],
    ["validate number", 123, false],
    ["validate null", null, false],
    ["validate object", {}, false],
    ["validate wrong length", uuid.NIL + "x", false],
    ["validate empty string", "", false],
  ];
  for (const [label, v, expected] of badCases) assertEq(uuid.validate(v), expected, label);
  const refusals = [
    ["parse malformed", () => uuid.parse("xyz")],
    ["parse empty", () => uuid.parse("")],
    ["parse wrong length", () => uuid.parse(uuid.NIL.slice(1))],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
}

// ============================ version / variant (d.ts L5981-5984) ============================
// "The version nibble; throws on a malformed string." / "'NCS', 'RFC4122', 'Microsoft' or 'Future'."
{
  const cases = [
    ["version(NIL)", uuid.NIL, 0],
    ["version(MAX)", uuid.MAX, 15],
    ["version(v4)", uuid.v4(), 4],
    ["version(v7)", uuid.v7(), 7],
  ];
  for (const [label, u, expected] of cases) assertEq(uuid.version(u), expected, label);
  const vcases = [
    ["variant(NIL) all-zero bits", uuid.NIL, "NCS"],
    ["variant(MAX) all-one bits", uuid.MAX, "Future"],
    ["variant(v4)", uuid.v4(), "RFC4122"],
    ["variant(v7)", uuid.v7(), "RFC4122"],
  ];
  for (const [label, u, expected] of vcases) assertEq(uuid.variant(u), expected, label);
  assertThrows(() => uuid.version("nope"), "version throws on a malformed string");
}

// ============================ bytes / fromBytes (d.ts L5985-5988) ============================
// "The 16 raw bytes of a parsed UUID." / "The canonical string for exactly 16 bytes."
{
  assert(eqArr([...uuid.bytes(uuid.NIL)], new Array(16).fill(0)), "bytes(NIL) is 16 zero bytes");
  assert(eqArr([...uuid.bytes(uuid.MAX)], new Array(16).fill(255)), "bytes(MAX) is 16 0xFF bytes");
  assertEq(uuid.fromBytes(new Uint8Array(16)), uuid.NIL, "fromBytes(16 zeros) = NIL");
  assertEq(uuid.fromBytes(new Uint8Array(16).fill(255)), uuid.MAX, "fromBytes(16 x 0xFF) = MAX");
  const s = uuid.v4();
  assertEq(uuid.fromBytes(uuid.bytes(s)), s, "fromBytes/bytes round-trip");
  const dv = new DataView(new ArrayBuffer(16));
  assertEq(typeof uuid.fromBytes(dv), "string", "fromBytes accepts any ByteView (DataView door)");
  assertThrows(() => uuid.fromBytes(new Uint8Array(15)), "fromBytes refuses wrong length");
}

// ============================ NanoID / NanoIDAlphabet (d.ts L5989-5992) ============================
// "URL-safe ID over the default 64-symbol alphabet; size 1..4096." / "alphabet of 2..256 ASCII symbols."
{
  const urlSafe = (s) => /^[A-Za-z0-9_-]+$/.test(s);
  const def = uuid.NanoID();
  assert(typeof def === "string" && def.length >= 1 && urlSafe(def), "NanoID() default is a non-empty URL-safe id");
  const cases = [
    ["NanoID(1)", 1], ["NanoID(21)", 21], ["NanoID(4096) at the cap", 4096],
  ];
  for (const [label, size] of cases) {
    const id = uuid.NanoID(size);
    assertEq(id.length, size, label + " length");
    assert(urlSafe(id), label + " is URL-safe");
  }
  const ab = uuid.NanoIDAlphabet("ab", 10);
  assertEq(ab.length, 10, "NanoIDAlphabet size honoured");
  for (const ch of ab) assert(ch === "a" || ch === "b", "NanoIDAlphabet char '" + ch + "' comes from the given alphabet");
  assertEq(uuid.NanoIDAlphabet("ab").length >= 1, true, "NanoIDAlphabet size is optional");
  const refusals = [
    ["NanoID(0) below 1..4096", () => uuid.NanoID(0)],
    ["NanoID(-1)", () => uuid.NanoID(-1)],
    ["NanoID(4097) above 1..4096", () => uuid.NanoID(4097)],
    ["NanoIDAlphabet '' below 2..256", () => uuid.NanoIDAlphabet("", 4)],
    ["NanoIDAlphabet 1 symbol", () => uuid.NanoIDAlphabet("a", 4)],
    ["NanoIDAlphabet 257 symbols", () => uuid.NanoIDAlphabet("x".repeat(257), 4)],
    ["NanoIDAlphabet non-ASCII symbol", () => uuid.NanoIDAlphabet("a\u00e9", 4)],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
}

// ============================ ULID / ULIDTime (d.ts L5993-6002) ============================
// "A 26-character Crockford base32 ULID; atMillis must fit 48 bits. {monotonic: true}: ... rapid same-ms
//  calls are strictly ascending ... Default is fresh entropy per call: NOT monotonic."
{
  const u = uuid.ULID();
  assertEq(u.length, 26, "ULID() is 26 characters");
  assert(ULID_RE.test(u), "ULID() is Crockford base32 (no I, L, O, U)");
  assert(Number.isInteger(uuid.ULIDTime(u)) && uuid.ULIDTime(u) >= 0, "ULIDTime(default ULID) is a non-negative integer ms");
  const cases = [
    ["ULID(123456789012) ms round-trip", 123456789012, 123456789012],
    ["ULID(0n) bigint door round-trip", 0n, 0],
    ["ULID(2^48-1) top of the 48-bit field", Math.pow(2, 48) - 1, Math.pow(2, 48) - 1],
  ];
  for (const [label, atMillis, expected] of cases) {
    assertEq(uuid.ULIDTime(uuid.ULID(atMillis)), expected, label);
  }
  assert(typeof uuid.ULID(123456789012, { monotonic: true }) === "string", "monotonic option accepted");
  assertThrows(() => uuid.ULID(Math.pow(2, 48)), "atMillis not fitting 48 bits is refused");
  // monotonic mode: rapid same-ms calls are strictly ascending
  const mono = [];
  for (let i = 0; i < 30; i++) mono.push(uuid.ULID(1700000000000, { monotonic: true }));
  for (let i = 1; i < mono.length; i++) assert(mono[i] > mono[i - 1], "monotonic ULID " + i + " strictly ascending at a fixed ms");
  for (const m of mono) assertEq(uuid.ULIDTime(m), 1700000000000, "monotonic ULID keeps the pinned millisecond");
}

print("bb_uuid: all tests passed (" + n + " assertions)");
