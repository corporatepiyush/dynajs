/* test_uuid.js -- dyna:uuid (RFC 9562 UUID v4/v7/v3/v5, parse/validate/
 * version/variant, bytes<->string, NIL/MAX, predefined namespaces).
 *
 * v3/v5 name-based vectors are cross-checked against Python's `uuid` stdlib
 * module (uuid.uuid3/uuid.uuid5, an independent oracle) -- the same
 * external-oracle strategy tests/test_encoding.js uses against Python's
 * base64. The well-known RFC 9562 appendix vector (namespace DNS +
 * "www.example.com") is included: v5 = 2ed6657d-e927-568b-95e1-2665a8aea6a2,
 * v3 = 5df41881-3aed-3515-88a7-2f4a814cf09e.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_uuid.js
 * Prints "test_uuid: all tests passed (N assertions)" on success; throws on
 * failure. */

import {
    v4, v7, v3, v5, parse, validate, version, variant, bytes, fromBytes,
    NIL, MAX, NAMESPACE_DNS, NAMESPACE_URL, NAMESPACE_OID, NAMESPACE_X500,
} from "dyna:uuid";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + " (got " + got + ", want " + want + ")");
}
function assertThrows(fn, msg, ErrType) {
    n++;
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
    if (ErrType && !(err instanceof ErrType))
        throw new Error("assertion failed (wrong error type, got " + err + "): " + msg);
}

const CANON = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const VARIANT_CH = { "8": 1, "9": 1, "a": 1, "b": 1 }; /* variant "10xx" high nibble */

function extractMs(u) {
    /* bytes 0..5 (48-bit big-endian ms) = hex chars [0..7] + [9..12]. */
    return parseInt(u.slice(0, 8) + u.slice(9, 13), 16);
}

/* ============================== NIL / MAX ============================== */
{
    assertEq(NIL, "00000000-0000-0000-0000-000000000000", "NIL literal");
    assertEq(MAX, "ffffffff-ffff-ffff-ffff-ffffffffffff", "MAX literal");
    assert(validate(NIL), "validate(NIL)");
    assert(validate(MAX), "validate(MAX)");
    assertEq(version(NIL), 0, "version(NIL)");
    assertEq(version(MAX), 15, "version(MAX)");
    assertEq(variant(NIL), "NCS", "variant(NIL)");
    assertEq(variant(MAX), "Future", "variant(MAX)");
    assertEq(parse(NIL), NIL, "parse(NIL) idempotent");
    assertEq(parse(MAX), MAX, "parse(MAX) idempotent");
    const nb = bytes(NIL), xb = bytes(MAX);
    assert(nb instanceof Uint8Array && nb.length === 16, "bytes(NIL) is Uint8Array(16)");
    for (let i = 0; i < 16; i++) { assertEq(nb[i], 0x00, "NIL byte " + i); assertEq(xb[i], 0xff, "MAX byte " + i); }
    assertEq(fromBytes(nb), NIL, "fromBytes(bytes(NIL))");
    assertEq(fromBytes(xb), MAX, "fromBytes(bytes(MAX))");
}

/* ====================== predefined namespace constants ================= */
{
    assertEq(NAMESPACE_DNS, "6ba7b810-9dad-11d1-80b4-00c04fd430c8", "NAMESPACE_DNS");
    assertEq(NAMESPACE_URL, "6ba7b811-9dad-11d1-80b4-00c04fd430c8", "NAMESPACE_URL");
    assertEq(NAMESPACE_OID, "6ba7b812-9dad-11d1-80b4-00c04fd430c8", "NAMESPACE_OID");
    assertEq(NAMESPACE_X500, "6ba7b814-9dad-11d1-80b4-00c04fd430c8", "NAMESPACE_X500");
    for (const ns of [NAMESPACE_DNS, NAMESPACE_URL, NAMESPACE_OID, NAMESPACE_X500])
        assert(validate(ns), "namespace valid: " + ns);
    /* version 1, variant RFC4122 for all four */
    assertEq(version(NAMESPACE_DNS), 1, "NAMESPACE_DNS is v1");
    assertEq(variant(NAMESPACE_DNS), "RFC4122", "NAMESPACE_DNS variant");
}

/* ============================== v4: 100k ============================== */
{
    const N = 100000;
    const seen = new Set();
    for (let i = 0; i < N; i++) {
        const u = v4();
        if (!CANON.test(u)) throw new Error("v4 not canonical: " + u);
        if (u[14] !== "4") throw new Error("v4 version char != 4: " + u);
        if (!VARIANT_CH[u[19]]) throw new Error("v4 variant bits != 10xx: " + u);
        if (!validate(u)) throw new Error("v4 does not validate: " + u);
        seen.add(u);
    }
    n += 4 * N; /* four inline checks per generated uuid, above */
    assertEq(seen.size, N, "v4 uniqueness (no collisions in " + N + ")");

    /* native round-trips on a sample */
    let it = 0;
    for (const u of seen) {
        if (it++ >= 500) break;
        assertEq(parse(u), u, "v4 parse idempotent");
        assertEq(version(u), 4, "v4 version()");
        assertEq(variant(u), "RFC4122", "v4 variant()");
        assertEq(fromBytes(bytes(u)), u, "v4 bytes<->string round-trip");
    }
}

/* ============================== v7: 100k ============================== */
{
    const N = 100000;
    const seen = new Set();
    const t0 = Date.now();
    let prevMs = 0;
    for (let i = 0; i < N; i++) {
        const u = v7();
        if (!CANON.test(u)) throw new Error("v7 not canonical: " + u);
        if (u[14] !== "7") throw new Error("v7 version char != 7: " + u);
        if (!VARIANT_CH[u[19]]) throw new Error("v7 variant bits != 10xx: " + u);
        if (!validate(u)) throw new Error("v7 does not validate: " + u);
        const ms = extractMs(u);
        if (ms < prevMs) throw new Error("v7 embedded ms decreased: " + prevMs + " -> " + ms);
        prevMs = ms;
        seen.add(u);
    }
    const t1 = Date.now();
    n += 5 * N; /* five inline checks per generated uuid (incl. non-decreasing ms) */
    assertEq(seen.size, N, "v7 uniqueness (no collisions in " + N + ")");

    /* the embedded timestamp tracks wall-clock ms (generous 5s window) */
    let it = 0;
    for (const u of seen) {
        if (it++ >= 1000) break;
        const ms = extractMs(u);
        assert(ms >= t0 - 5000 && ms <= t1 + 5000,
            "v7 embedded ms ~ Date.now(): ms=" + ms + " window=[" + t0 + "," + t1 + "]");
        assertEq(version(u), 7, "v7 version()");
        assertEq(variant(u), "RFC4122", "v7 variant()");
        assertEq(fromBytes(bytes(u)), u, "v7 bytes<->string round-trip");
    }
}

/* ====================== parse: the four accepted forms ================= */
{
    const canon = v4();                                  /* a fresh known-good uuid */
    const upper = canon.toUpperCase();
    const raw   = canon.replace(/-/g, "");               /* 32 hex, no hyphens */
    const braced = "{" + canon + "}";                    /* 38 */
    const urn   = "urn:uuid:" + canon;                   /* 45 */
    const urnUp = "URN:UUID:" + upper;                   /* case-insensitive prefix */

    assertEq(parse(canon), canon, "parse canonical");
    assertEq(parse(upper), canon, "parse uppercase -> lowercase canonical");
    assertEq(parse(raw), canon, "parse 32-char raw hex");
    assertEq(parse(braced), canon, "parse braced");
    assertEq(parse(urn), canon, "parse urn:uuid:");
    assertEq(parse(urnUp), canon, "parse URN:UUID: (case-insensitive)");
    for (const f of [canon, upper, raw, braced, urn, urnUp])
        assert(validate(f), "validate accepts form: " + f);
}

/* ========================= parse: malformed battery =================== */
{
    const bad = [
        "",                                                   /* empty */
        "xyz",                                                /* short junk */
        "00000000-0000-0000-0000-00000000000",                /* 35: too short */
        "00000000-0000-0000-0000-0000000000000",              /* 37: too long */
        "00000000-0000-0000-0000-000000000000-",              /* 37 trailing hyphen */
        "00000000-000-00000-0000-000000000000",               /* 36, hyphens misplaced */
        "0000000g-0000-0000-0000-000000000000",               /* non-hex digit */
        "0000000000000000000000000000000g",                   /* 32-char with non-hex */
        "000000000000000000000000000000000",                  /* 33 raw */
        "0000000000000000000000000000000",                    /* 31 raw */
        "{00000000-0000-0000-0000-000000000000",              /* 37: only open brace */
        "00000000-0000-0000-0000-000000000000}",              /* 37: only close brace */
        "[00000000-0000-0000-0000-000000000000]",             /* 38 but not braces */
        "urn:uxid:00000000-0000-0000-0000-000000000000",      /* 45 wrong prefix */
        "00000000-0000-0000-0000-00000000000 ",               /* 36 with trailing space */
        " 0000000-0000-0000-0000-000000000000 ",              /* 36 with spaces */
    ];
    for (const s of bad) {
        assert(!validate(s), "validate rejects: '" + s + "'");
        assertThrows(() => parse(s), "parse throws on: '" + s + "'", SyntaxError);
    }
    /* non-string arguments: validate -> false (never throws) */
    for (const v of [123, null, undefined, {}, [], true, 0, NaN, new String(NIL)])
        assert(!validate(v), "validate(non-string) is false: " + String(v));
}

/* =================== version()/variant() classification =============== */
{
    /* craft raw byte-8 values to exercise every variant branch via fromBytes */
    function withByte8(hi) {
        const b = new Uint8Array(16);
        b[8] = hi;
        return fromBytes(b);
    }
    assertEq(variant(withByte8(0x00)), "NCS", "variant 0xxx -> NCS");
    assertEq(variant(withByte8(0x70)), "NCS", "variant 0111.. still NCS (top bit 0)");
    assertEq(variant(withByte8(0x80)), "RFC4122", "variant 10xx -> RFC4122");
    assertEq(variant(withByte8(0xb0)), "RFC4122", "variant 1011 -> RFC4122");
    assertEq(variant(withByte8(0xc0)), "Microsoft", "variant 110x -> Microsoft");
    assertEq(variant(withByte8(0xd0)), "Microsoft", "variant 1101 -> Microsoft");
    assertEq(variant(withByte8(0xe0)), "Future", "variant 111x -> Future");
    assertEq(variant(withByte8(0xf0)), "Future", "variant 1111 -> Future");
    /* version nibble from byte 6 */
    function withByte6(hi) {
        const b = new Uint8Array(16);
        b[6] = hi << 4;
        return fromBytes(b);
    }
    for (let ver = 0; ver <= 15; ver++)
        assertEq(version(withByte6(ver)), ver, "version nibble " + ver);
}

/* ===================== v3 (MD5) / v5 (SHA-1) vectors ================== */
/* Cross-checked against Python: uuid.uuid5 / uuid.uuid3 (see header). */
{
    const V5 = [
        [NAMESPACE_DNS, "www.example.com", "2ed6657d-e927-568b-95e1-2665a8aea6a2"],
        [NAMESPACE_DNS, "example.org",     "aad03681-8b63-5304-89e0-8ca8f49461b5"],
        [NAMESPACE_DNS, "hello",           "9342d47a-1bab-5709-9869-c840b2eac501"],
        [NAMESPACE_URL, "www.example.com", "b63cdfa4-3df9-568e-97ae-006c5b8fd652"],
        [NAMESPACE_URL, "example.org",     "54a35416-963c-5dd6-a1e2-5ab7bb5bafc7"],
        [NAMESPACE_URL, "hello",           "074171de-bc84-5ea4-b636-1135477620e1"],
    ];
    const V3 = [
        [NAMESPACE_DNS, "www.example.com", "5df41881-3aed-3515-88a7-2f4a814cf09e"],
        [NAMESPACE_DNS, "example.org",     "04738bdf-b25a-3829-a801-b21a1d25095b"],
        [NAMESPACE_DNS, "hello",           "0bacede4-4014-3f9d-b720-173f68a1c933"],
        [NAMESPACE_URL, "www.example.com", "a777199a-c522-31c4-8f4b-335feec7215b"],
        [NAMESPACE_URL, "example.org",     "39682ca1-9168-3da2-a1bb-f4dbcde99bf9"],
        [NAMESPACE_URL, "hello",           "cf3741de-a2dd-36f7-a791-8736e42c4c2f"],
    ];
    for (const [ns, name, want] of V5) {
        assertEq(v5(ns, name), want, "v5(" + ns + "," + name + ")");
        assertEq(version(want), 5, "v5 result version==5");
        assertEq(variant(want), "RFC4122", "v5 result variant");
    }
    for (const [ns, name, want] of V3) {
        assertEq(v3(ns, name), want, "v3(" + ns + "," + name + ")");
        assertEq(version(want), 3, "v3 result version==3");
        assertEq(variant(want), "RFC4122", "v3 result variant");
    }

    /* determinism + namespace-as-bytes + name-as-Uint8Array equivalence */
    assertEq(v5(NAMESPACE_DNS, "hello"), v5(NAMESPACE_DNS, "hello"), "v5 deterministic");
    const nsBytes = bytes(NAMESPACE_DNS);
    assertEq(v5(nsBytes, "www.example.com"), "2ed6657d-e927-568b-95e1-2665a8aea6a2",
        "v5 accepts a 16-byte namespace view");
    const nameBytes = new Uint8Array([0x77, 0x77, 0x77, 0x2e, 0x65, 0x78, 0x61, 0x6d,
                                      0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d]); /* "www.example.com" */
    assertEq(v5(NAMESPACE_DNS, nameBytes), "2ed6657d-e927-568b-95e1-2665a8aea6a2",
        "v5 accepts a byte-view name");
    assertEq(v3(nsBytes, nameBytes), "5df41881-3aed-3515-88a7-2f4a814cf09e",
        "v3 accepts byte-view namespace + name");

    /* empty name is valid (hashes just the namespace bytes) */
    assert(CANON.test(v5(NAMESPACE_DNS, "")), "v5 empty name is canonical");
    assert(CANON.test(v3(NAMESPACE_DNS, "")), "v3 empty name is canonical");

    /* a bad namespace throws; a non-string/non-view name throws TypeError */
    assertThrows(() => v5("not-a-uuid", "x"), "v5 bad namespace throws", SyntaxError);
    assertThrows(() => v5(NAMESPACE_DNS, {}), "v5 object name throws", TypeError);
    assertThrows(() => v5(new Uint8Array(15), "x"), "v5 15-byte namespace throws", TypeError);
}

/* ========================== fromBytes edge cases ====================== */
{
    assertThrows(() => fromBytes(new Uint8Array(15)), "fromBytes(15) throws", RangeError);
    assertThrows(() => fromBytes(new Uint8Array(17)), "fromBytes(17) throws", RangeError);
    assertThrows(() => fromBytes("nope"), "fromBytes(string) throws", TypeError);
    /* an ArrayBuffer and an Int8Array view are both accepted (1-byte elements) */
    const ab = new ArrayBuffer(16);
    new Uint8Array(ab).fill(0xab);
    assertEq(fromBytes(ab), "abababab-abab-abab-abab-abababababab",
        "fromBytes(ArrayBuffer)");
    assertEq(fromBytes(new Int8Array(16)), NIL, "fromBytes(Int8Array of zeros)");
}

print("test_uuid: all tests passed (" + n + " assertions)");
